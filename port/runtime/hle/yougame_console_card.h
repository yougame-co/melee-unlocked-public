// Bounded Melee GCI transfer and relaunch staging. No other card contents are read.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "yougame_console.h"
#include "../host/hackpack_ai.h"
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>

namespace yougame::console {
// The source host installs this at load (port/app/source_host.cpp): the actual mounted Melee save only.
// The runtime libraries are built without MELEE_SOURCE_PORT, so they must not call the host directly.
using HostSaveReader = std::vector<uint8_t> (*)();
inline std::atomic<HostSaveReader> host_save_reader{nullptr};
inline constexpr const char* kCardFailure = "Couldn't get your friend's save";
// A 2-seat session whose start carries no cardDigest: every seat boots the same fresh card.
inline constexpr const char* kDefaultCard = "Couldn't share the host's save, playing with default unlocks";
inline constexpr size_t kCardLimit = 2 * 1024 * 1024;
// 12 KiB base64 alone fills Rooms' 16 KiB limit; leave space for the JSON envelope.
inline constexpr size_t kCardChunk = 11 * 1024;
inline std::string card_sha(const std::vector<uint8_t>& b) {
  return host::hackpack_ai::rules::sha256_hex(b.data(), b.size());
}
inline bool card_hash(const std::string& s) {
  return s.size() == 64 && s.find_first_not_of("0123456789abcdef") == std::string::npos;
}
inline bool melee_gci(const std::vector<uint8_t>& b) {
  constexpr char name[] = "SuperSmashBros0110290334";
  return b.size() >= 64 + 8192 && b.size() <= kCardLimit &&
      std::equal(b.begin(), b.begin()+6, "GALE01") &&
      std::equal(name, name+sizeof(name), b.begin()+8) &&
      64 + size_t((b[0x38] << 8) | b[0x39]) * 8192 == b.size();
}
inline std::string card_encode(const uint8_t* b, size_t size) {
  constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string s;
  for (size_t n = 0; n < size; n += 3) {
    const uint32_t v = uint32_t(b[n]) << 16 | (n+1 < size ? uint32_t(b[n+1]) << 8 : 0) | (n+2 < size ? b[n+2] : 0);
    s += alphabet[v >> 18]; s += alphabet[(v >> 12) & 63];
    s += n+1 < size ? alphabet[(v >> 6) & 63] : '=';
    s += n+2 < size ? alphabet[v & 63] : '=';
  }
  return s;
}
inline bool card_decode(const std::string& s, std::vector<uint8_t>& b) {
  if (s.empty() || s.size() > ((kCardChunk+2)/3)*4 || s.size()%4) return false;
  constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  b.clear();
  for (size_t n = 0; n < s.size(); n += 4) {
    uint32_t v = 0; int pad = 0;
    for (int j = 0; j < 4; ++j) {
      const char ch = s[n+j];
      if (ch == '=') { if (j < 2 || n+4 != s.size()) return false; ++pad; v <<= 6; }
      else {
        const auto at = std::string(alphabet).find(ch);
        if (pad || at == std::string::npos) return false;
        v = (v << 6) | uint32_t(at);
      }
    }
    if (pad > 2 || (pad == 2 && (v & 0xffff)) || (pad == 1 && (v & 0xff))) return false;
    b.push_back(uint8_t(v >> 16));
    if (pad < 2) b.push_back(uint8_t(v >> 8));
    if (!pad) b.push_back(uint8_t(v));
  }
  return b.size() <= kCardChunk;
}
inline bool card_bytes_ok(const SessionConfig& c) {
  return melee_gci(c.card_bytes) && card_hash(c.card_digest) && card_sha(c.card_bytes) == c.card_digest;
}
// Traverse only fresh project-owned session paths. Reject symlinks at every component.
inline bool card_session_dir(const std::filesystem::path& data, const std::string& id,
                             std::filesystem::path& path) {
  if (data.empty() || id.empty() || id == "." || id == ".." || id.size() > 64 ||
      id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos) return false;
  std::error_code ec;
  if (!std::filesystem::is_directory(data, ec) || ec) return false;
  path = data;
  for (const auto& part : {std::string("console"), id}) {
    path /= part;
    auto status = std::filesystem::symlink_status(path, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) return false;
    ec.clear();
    if (std::filesystem::is_symlink(status)) return false;
    if (!std::filesystem::exists(status)) std::filesystem::create_directory(path, ec);
    if (ec || !std::filesystem::is_directory(path, ec) || ec) return false;
  }
  return true;
}
inline bool stage_card(const std::filesystem::path& data, const SessionConfig& c) {
  std::filesystem::path dir;
  if (!card_bytes_ok(c) || !card_session_dir(data,c.session_id,dir)) return false;
  std::error_code ec; auto path = dir / "incoming.gci";
  const auto status = std::filesystem::symlink_status(path,ec);
  if (ec && ec != std::errc::no_such_file_or_directory) return false;
  if (std::filesystem::exists(status) || std::filesystem::is_symlink(status)) return false;
  std::ofstream out(path,std::ios::binary);
  out.write(reinterpret_cast<const char*>(c.card_bytes.data()),c.card_bytes.size()); out.close();
  return bool(out);
}
inline bool load_card(const std::filesystem::path& data, SessionConfig& c) {
  std::filesystem::path dir;
  if (!card_hash(c.card_digest) || !card_session_dir(data,c.session_id,dir)) return false;
  const auto path = dir / "incoming.gci"; std::error_code ec;
  if (std::filesystem::is_symlink(path,ec) || !std::filesystem::is_regular_file(path,ec)) return false;
  const auto size = std::filesystem::file_size(path,ec);
  if (ec || size > kCardLimit || size < 64) return false;
  std::ifstream in(path,std::ios::binary); c.card_bytes.resize(size);
  in.read(reinterpret_cast<char*>(c.card_bytes.data()),size);
  return bool(in) && card_bytes_ok(c);
}
} // namespace yougame::console
