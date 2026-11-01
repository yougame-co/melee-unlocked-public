// Process-independent snapshot container. No platform APIs or game calls.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "native_savestate.h"
#include <array>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace handoff_state {
struct Range { uintptr_t address = 0; uint32_t size = 0; };
// Strict hexadecimal address; no signs, whitespace, overflow, or empty ranges.
inline bool parse_range(const std::string& address, uint64_t size, Range& out) {
  if (address.size() < 3 || address.compare(0, 2, "0x") || !size || size > UINT32_MAX) return false;
  uintptr_t n = 0;
  for (size_t i = 2; i < address.size(); ++i) {
    const char c = address[i];
    const unsigned digit = c >= '0' && c <= '9' ? c-'0' :
        c >= 'a' && c <= 'f' ? c-'a'+10 : c >= 'A' && c <= 'F' ? c-'A'+10 : 16;
    if (digit > 15 || n > (UINTPTR_MAX-digit)/16) return false;
    n = n*16 + digit;
  }
  if (!n || size > UINTPTR_MAX-n) return false;
  out = {n, static_cast<uint32_t>(size)};
  return true;
}
bool validate_keep(const std::vector<Range>&, const std::vector<native_savestate::Span>&,
                   std::string& reason);
struct Identity {
  uint64_t size = 0;
  std::array<uint8_t, 32> sha256{};
};
struct State {
  uint64_t frame = 0;
  uint32_t scene_major = 0, scene_minor = 0;
  // Reserved for an explicitly encoded, versioned host-state record. Never copy native structs
  // containing pointers/padding here. The live host integration must validate it before load().
  std::vector<uint8_t> host;
};
struct Sizes { uint64_t raw = 0, stored = 0, file = 0; };
struct Gate {
  uint64_t frame = 0;
  uint32_t scene_major = 0, scene_minor = 0;
  bool disc_pending = false, callback_pending = false;
};
bool validate_gate(const State&, const Gate&, std::string& reason);
constexpr size_t header_size = 124, table_entry_size = 12;
constexpr size_t max_host_size = 64 * 1024;
constexpr uint64_t max_raw_size = 128ull * 1024 * 1024;

// Fixed little-endian format v1:
// MUHS, u32 version, u64 library bytes, SHA256[32], u32 span count, u64 frame,
// u32 scene major/minor, u32 host length, u32 codec (0 raw, 1 zlib), u64 raw/stored length,
// SHA256[32] of host record + uncompressed span bytes, then (u64 address,u32 size)[count],
// host record, payload. Addresses identify spans; the reader NEVER dereferences file addresses.
// Compression uses the tree's vendored stb zlib implementation, falling back to raw if larger.
// Bound the file before allocating; validate everything before changing ANY live span or State.
bool encode(const Identity&, const std::vector<native_savestate::Span>&, const State&,
            std::vector<uint8_t>& file, Sizes&, std::string& reason, bool compress = true);
// Frame-boundary copy. Only owned bytes and numeric addresses cross to the worker.
struct Capture {
  Identity identity;
  State state;
  std::vector<Range> spans;
  std::vector<uint8_t> bytes;
};
bool capture(const Identity&, const std::vector<native_savestate::Span>&, const State&,
             Capture&, std::string& reason);
bool encode_capture(const Capture&, std::vector<uint8_t>& file, Sizes&, std::string& reason,
                    bool compress = true);
// Decode and validate without touching any live memory. raw is the concatenated span payload.
bool inspect(const std::vector<uint8_t>&, const Identity&,
             const std::vector<native_savestate::Span>&, State&, Sizes&,
             std::vector<uint8_t>& raw, std::string& reason);
constexpr size_t join_stored_limit = 24 * 1024 * 1024, delta_page_size = 4096;
// MUHP v1: base SHA256, decoded/stored patch lengths, patch SHA256, zlib patch.
// Patch: target MUHS prefix, followed by strictly ordered changed 4 KiB span-payload pages.
bool encode_delta(const std::vector<uint8_t>& base, const Identity&,
                  const std::vector<native_savestate::Span>&, const State&,
                  std::vector<uint8_t>& delta, size_t& pages, std::string& reason);
bool apply_delta(const std::vector<uint8_t>& base, const std::vector<uint8_t>& delta,
                 const Identity&, const std::vector<native_savestate::Span>&,
                 std::vector<uint8_t>& target, std::string& reason, size_t* pages = nullptr);
bool load(const std::vector<uint8_t>& file, const Identity&,
          const std::vector<native_savestate::Span>&, State&, Sizes&, std::string& reason,
          const Gate* gate = nullptr, const std::vector<Range>& keep = {});

// FILE* ownership stays with the caller (so hooks can use directory-confined capabilities).
// read_file caps actual bytes, including files that grow after being opened.
bool read_file(FILE*, const std::vector<native_savestate::Span>&,
               std::vector<uint8_t>&, std::string& reason);
bool write_file(FILE*, const std::vector<uint8_t>&, std::string& reason);
// Call once after loading the library and retain the result for the session.
bool library_identity(const std::filesystem::path&, Identity&, std::string& reason);

// MUHD, u32 version=1, u32 count, u64 raw size, the same span table, then RAW span bytes.
// tools/handoff_diff.py consumes this diagnostic format; it is not a loadable snapshot.
bool dump(FILE*, const std::vector<native_savestate::Span>&, std::string& reason);
} // namespace handoff_state
