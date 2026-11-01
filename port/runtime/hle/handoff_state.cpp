// SPDX-License-Identifier: GPL-2.0-or-later
#include "handoff_state.h"
#include "../host/hackpack_ai.h" // existing portable SHA-256 implementation
#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>

// Private copies: no stb symbols/settings collide with the renderer's copies. This only uses
// the already-vendored zlib codec; no new dependency, file access or network facility is added.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_ZLIB
#define STBI_SUPPORT_ZLIB
#define STBI_NO_STDIO
#include "../../third_party/stb/stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "../../third_party/stb/stb_image_write.h"

namespace handoff_state {
namespace {
using native_savestate::Span;
bool refuse(std::string& reason, const char* text) { reason = text; return false; }
std::array<uint8_t, 32> digest(const uint8_t* p, size_t n) {
  const auto hex = host::hackpack_ai::rules::sha256_hex(p, n);
  std::array<uint8_t, 32> out{};
  auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  for (size_t i = 0; i < out.size(); ++i) out[i] = uint8_t(nibble(hex[2*i])*16 + nibble(hex[2*i+1]));
  return out;
}
void put(std::vector<uint8_t>& out, uint64_t value, unsigned bytes) {
  for (unsigned i = 0; i < bytes; ++i) out.push_back(uint8_t(value >> (8*i)));
}
uint64_t get(const std::vector<uint8_t>& in, size_t at, unsigned bytes) {
  uint64_t n = 0;
  for (unsigned i = 0; i < bytes; ++i) n |= uint64_t(in[at+i]) << (8*i);
  return n;
}
bool layout(const std::vector<Span>& spans, uint64_t& raw, std::string& reason) {
  raw = 0;
  if (spans.empty() || spans.size() > 65536) return refuse(reason, "invalid span count");
  uintptr_t end = 0;
  for (const auto& s : spans) {
    const auto a = reinterpret_cast<uintptr_t>(s.address);
    if (!a || !s.size || a < end || s.size > std::numeric_limits<uintptr_t>::max() - a)
      return refuse(reason, "invalid or unordered live span table");
    end = a + s.size;
    raw += s.size;
    if (raw > max_raw_size) return refuse(reason, "snapshot exceeds size limit");
  }
  return true;
}
void table(std::vector<uint8_t>& out, const std::vector<Span>& spans) {
  for (const auto& s : spans) { put(out, reinterpret_cast<uintptr_t>(s.address), 8); put(out, s.size, 4); }
}
} // namespace

bool validate_keep(const std::vector<Range>& keep, const std::vector<Span>& spans, std::string& reason) {
  for (const auto& r : keep) {
    if (!r.address || !r.size || r.size > UINTPTR_MAX-r.address)
      return refuse(reason, "invalid handoffKeep range");
    uintptr_t cursor = r.address;
    const auto end = r.address + r.size;
    for (const auto& s : spans) {
      const auto lo = reinterpret_cast<uintptr_t>(s.address);
      if (lo <= cursor && cursor-lo < s.size) cursor += std::min<uintptr_t>(end-cursor, s.size-(cursor-lo));
      if (cursor == end) break;
    }
    if (cursor != end) return refuse(reason, "handoffKeep range outside snapshot spans");
  }
  return true;
}
bool validate_gate(const State& state, const Gate& gate, std::string& reason) {
  if (gate.disc_pending) return refuse(reason, "disc read/callback pending");
  if (gate.callback_pending) return refuse(reason, "guest callback pending");
  if (state.frame != gate.frame) return refuse(reason, "different frame count");
  if (state.scene_major != gate.scene_major || state.scene_minor != gate.scene_minor)
    return refuse(reason, "different scene");
  if (!state.host.empty()) return refuse(reason, "unsupported host state record");
  return true;
}

bool encode(const Identity& id, const std::vector<Span>& spans, const State& state,
            std::vector<uint8_t>& file, Sizes& sizes, std::string& reason, bool compress) {
  reason.clear(); file.clear(); sizes = {};
  uint64_t raw = 0;
  if (!layout(spans, raw, reason)) return false;
  if (state.host.size() > max_host_size) return refuse(reason, "host state exceeds size limit");
  std::vector<uint8_t> checked = state.host;
  for (const auto& s : spans) checked.insert(checked.end(), s.address, s.address + s.size);
  const auto hash = digest(checked.data(), checked.size());
  auto* payload = checked.data() + state.host.size();
  int compressed_size = 0;
  unsigned char* compressed = compress ? stbi_zlib_compress(payload, static_cast<int>(raw), &compressed_size, 5) : nullptr;
  if (compress && !compressed) return refuse(reason, "compression failed");
  const bool zlib = compressed_size > 0 && uint64_t(compressed_size) < raw;
  const uint64_t stored = zlib ? uint64_t(compressed_size) : raw;
  file.insert(file.end(), {'M','U','H','S'}); put(file, 1, 4); put(file, id.size, 8);
  file.insert(file.end(), id.sha256.begin(), id.sha256.end());
  put(file, spans.size(), 4); put(file, state.frame, 8);
  put(file, state.scene_major, 4); put(file, state.scene_minor, 4);
  put(file, state.host.size(), 4); put(file, zlib ? 1 : 0, 4);
  put(file, raw, 8); put(file, stored, 8); file.insert(file.end(), hash.begin(), hash.end());
  table(file, spans); file.insert(file.end(), state.host.begin(), state.host.end());
  file.insert(file.end(), zlib ? compressed : payload, (zlib ? compressed : payload) + stored);
  STBIW_FREE(compressed);
  sizes = {raw, stored, file.size()};
  return true;
}

bool capture(const Identity& id, const std::vector<Span>& spans, const State& state,
             Capture& out, std::string& reason) {
  reason.clear();
  uint64_t raw = 0;
  if (!layout(spans,raw,reason)) return false;
  if (state.host.size() > max_host_size) return refuse(reason,"host state exceeds size limit");
  out.identity = id; out.state = state; out.spans.clear(); out.spans.reserve(spans.size());
  out.bytes.resize(state.host.size()+raw);
  std::copy(state.host.begin(),state.host.end(),out.bytes.begin());
  size_t at = state.host.size();
  for (const auto& s : spans) {
    out.spans.push_back({reinterpret_cast<uintptr_t>(s.address),s.size});
    std::memcpy(out.bytes.data()+at,s.address,s.size); at += s.size;
  }
  return true;
}
bool encode_capture(const Capture& captured, std::vector<uint8_t>& file, Sizes& sizes,
                    std::string& reason, bool compress) {
  reason.clear(); file.clear(); sizes = {};
  const auto& state = captured.state;
  uint64_t raw = 0; uintptr_t end = 0;
  if (captured.spans.empty() || captured.spans.size() > 65536 || state.host.size() > max_host_size)
    return refuse(reason,"invalid capture");
  for (const auto& s : captured.spans) {
    if (!s.address || !s.size || s.address < end || s.size > UINTPTR_MAX-s.address)
      return refuse(reason,"invalid capture spans");
    end = s.address+s.size; raw += s.size;
    if (raw > max_raw_size) return refuse(reason,"snapshot exceeds size limit");
  }
  if (captured.bytes.size() != state.host.size()+raw) return refuse(reason,"invalid capture size");
  const auto hash = digest(captured.bytes.data(),captured.bytes.size());
  const auto* payload = captured.bytes.data()+state.host.size();
  int compressed_size = 0;
  auto* compressed = compress ? stbi_zlib_compress(const_cast<unsigned char*>(payload),int(raw),&compressed_size,5) : nullptr;
  if (compress && !compressed) return refuse(reason,"compression failed");
  const bool zlib = compressed_size > 0 && uint64_t(compressed_size) < raw;
  const uint64_t stored = zlib ? uint64_t(compressed_size) : raw;
  file.insert(file.end(),{'M','U','H','S'}); put(file,1,4); put(file,captured.identity.size,8);
  file.insert(file.end(),captured.identity.sha256.begin(),captured.identity.sha256.end());
  put(file,captured.spans.size(),4); put(file,state.frame,8);
  put(file,state.scene_major,4); put(file,state.scene_minor,4);
  put(file,state.host.size(),4); put(file,zlib ? 1 : 0,4);
  put(file,raw,8); put(file,stored,8); file.insert(file.end(),hash.begin(),hash.end());
  for (const auto& s : captured.spans) { put(file,s.address,8); put(file,s.size,4); }
  file.insert(file.end(),state.host.begin(),state.host.end());
  file.insert(file.end(),zlib ? compressed : payload,(zlib ? compressed : payload)+stored);
  STBIW_FREE(compressed); sizes = {raw,stored,file.size()}; return true;
}

bool inspect(const std::vector<uint8_t>& file, const Identity& id, const std::vector<Span>& spans,
             State& state, Sizes& sizes, std::vector<uint8_t>& bytes, std::string& reason) {
  reason.clear(); sizes = {};
  uint64_t raw = 0;
  if (!layout(spans, raw, reason)) return false;
  if (file.size() < header_size) return refuse(reason, "truncated header");
  if (std::memcmp(file.data(), "MUHS", 4)) return refuse(reason, "wrong magic");
  if (get(file, 4, 4) != 1) return refuse(reason, "unsupported version");
  if (get(file, 8, 8) != id.size || !std::equal(id.sha256.begin(), id.sha256.end(), file.begin()+16))
    return refuse(reason, "different game library identity");
  if (get(file, 48, 4) != spans.size()) return refuse(reason, "different span count");
  const auto host_size = get(file, 68, 4), codec = get(file, 72, 4);
  const auto stored = get(file, 84, 8);
  if (host_size > max_host_size) return refuse(reason, "host state exceeds size limit");
  if (codec > 1) return refuse(reason, "unsupported codec");
  if (get(file, 76, 8) != raw) return refuse(reason, "different raw span size");
  if (!stored || stored > raw || (!codec && stored != raw)) return refuse(reason, "invalid stored size");
  const uint64_t expected = header_size + table_entry_size * spans.size() + host_size + stored;
  if (file.size() != expected) return refuse(reason, file.size() < expected ? "truncated file" : "oversized file");
  size_t pos = header_size;
  for (const auto& s : spans) {
    if (get(file, pos, 8) != reinterpret_cast<uintptr_t>(s.address) || get(file, pos+8, 4) != s.size)
      return refuse(reason, "different span table");
    pos += table_entry_size;
  }
  State restored;
  restored.frame = get(file, 52, 8);
  restored.scene_major = uint32_t(get(file, 60, 4)); restored.scene_minor = uint32_t(get(file, 64, 4));
  restored.host.assign(file.begin()+pos, file.begin()+pos+host_size);
  pos += host_size;
  std::vector<uint8_t> checked = restored.host;
  checked.resize(host_size + raw);
  auto* payload = checked.data() + host_size;
  if (codec) {
    if (stbi_zlib_decode_buffer(reinterpret_cast<char*>(payload), static_cast<int>(raw),
        reinterpret_cast<const char*>(file.data()+pos), static_cast<int>(stored)) != static_cast<int>(raw))
      return refuse(reason, "invalid compressed payload");
  } else std::memcpy(payload, file.data()+pos, static_cast<size_t>(raw));
  const auto hash = digest(checked.data(), checked.size());
  if (!std::equal(hash.begin(), hash.end(), file.begin()+92)) return refuse(reason, "payload checksum mismatch");
  bytes.assign(payload, payload+raw);
  state = std::move(restored); sizes = {raw, stored, file.size()};
  return true;
}

bool load(const std::vector<uint8_t>& file, const Identity& id, const std::vector<Span>& spans,
          State& state, Sizes& sizes, std::string& reason, const Gate* gate, const std::vector<Range>& keep) {
  std::vector<uint8_t> bytes;
  State restored;
  Sizes decoded;
  sizes = {};
  if (!validate_keep(keep, spans, reason) || !inspect(file,id,spans,restored,decoded,bytes,reason)) return false;
  if (gate && !validate_gate(restored, *gate, reason)) return false;
  // Allocate and collect B's values before committing; no failure can leave a partial load.
  std::vector<std::vector<uint8_t>> preserved;
  for (const auto& r : keep) {
    const auto* p = reinterpret_cast<const uint8_t*>(r.address);
    preserved.emplace_back(p, p+r.size);
  }
  // Commit only after every refusal check. No serialized pointer is ever used as a destination.
  const auto* payload = bytes.data();
  for (const auto& s : spans) { std::memcpy(s.address, payload, s.size); payload += s.size; }
  for (size_t i = 0; i < keep.size(); ++i)
    std::memcpy(reinterpret_cast<void*>(keep[i].address), preserved[i].data(), keep[i].size);
  state = std::move(restored); sizes = decoded;
  return true;
}

bool encode_delta(const std::vector<uint8_t>& base, const Identity& id, const std::vector<Span>& spans,
                  const State& state, std::vector<uint8_t>& delta, size_t& pages, std::string& reason) {
  delta.clear(); pages = 0;
  if (base.size() > join_stored_limit) return refuse(reason,"base oversized");
  State old; Sizes sizes; std::vector<uint8_t> raw, target;
  if (!inspect(base,id,spans,old,sizes,raw,reason) || !encode(id,spans,state,target,sizes,reason,false)) return false;
  const size_t prefix = target.size()-raw.size();
  std::vector<uint8_t> patch;
  put(patch,prefix,4); patch.insert(patch.end(),target.begin(),target.begin()+prefix);
  const size_t count_at = patch.size(); put(patch,0,4);
  for (size_t at = 0; at < raw.size(); at += delta_page_size) {
    const auto n = std::min(delta_page_size,raw.size()-at);
    if (std::equal(raw.begin()+at,raw.begin()+at+n,target.begin()+prefix+at)) continue;
    put(patch,at/delta_page_size,4); put(patch,n,4);
    patch.insert(patch.end(),target.begin()+prefix+at,target.begin()+prefix+at+n); ++pages;
  }
  for (int j = 0; j < 4; ++j) patch[count_at+j] = uint8_t(pages >> (8*j));
  int stored = 0;
  auto* zipped = stbi_zlib_compress(patch.data(),int(patch.size()),&stored,5);
  if (!zipped) return refuse(reason,"delta compression failed");
  delta = {'M','U','H','P'}; put(delta,1,4);
  const auto base_hash = digest(base.data(),base.size()), hash = digest(patch.data(),patch.size());
  delta.insert(delta.end(),base_hash.begin(),base_hash.end());
  put(delta,patch.size(),8); put(delta,stored,8); delta.insert(delta.end(),hash.begin(),hash.end());
  delta.insert(delta.end(),zipped,zipped+stored); STBIW_FREE(zipped);
  if (delta.size() > join_stored_limit) { delta.clear(); return refuse(reason,"delta oversized"); }
  return true;
}

bool apply_delta(const std::vector<uint8_t>& base, const std::vector<uint8_t>& delta,
                 const Identity& id, const std::vector<Span>& spans,
                 std::vector<uint8_t>& target, std::string& reason, size_t* pages) {
  target.clear();
  if (base.size() > join_stored_limit || delta.size() > join_stored_limit) return refuse(reason,"transfer oversized");
  State state; Sizes sizes; std::vector<uint8_t> raw;
  if (!inspect(base,id,spans,state,sizes,raw,reason)) return false;
  if (delta.size() < 88 || std::memcmp(delta.data(),"MUHP",4) || get(delta,4,4) != 1)
    return refuse(reason,"invalid delta header");
  const auto base_hash = digest(base.data(),base.size());
  if (!std::equal(base_hash.begin(),base_hash.end(),delta.begin()+8)) return refuse(reason,"wrong delta base");
  const auto decoded = get(delta,40,8), stored = get(delta,48,8);
  const auto max_patch = raw.size() + ((raw.size()+delta_page_size-1)/delta_page_size)*8 +
      header_size + table_entry_size*spans.size() + max_host_size + 8;
  if (!decoded || decoded > max_patch || !stored || stored != delta.size()-88)
    return refuse(reason,"invalid delta size");
  std::vector<uint8_t> patch(decoded);
  if (stbi_zlib_decode_buffer(reinterpret_cast<char*>(patch.data()),int(decoded),
      reinterpret_cast<const char*>(delta.data()+88),int(stored)) != int(decoded))
    return refuse(reason,"invalid delta compression");
  const auto hash = digest(patch.data(),patch.size());
  if (!std::equal(hash.begin(),hash.end(),delta.begin()+56)) return refuse(reason,"delta checksum mismatch");
  if (patch.size() < 8) return refuse(reason,"truncated delta prefix");
  const auto prefix = get(patch,0,4);
  if (prefix < header_size || prefix > header_size+table_entry_size*spans.size()+max_host_size || prefix > patch.size()-8)
    return refuse(reason,"invalid delta prefix");
  size_t at = 4+prefix;
  const auto count = get(patch,at,4); at += 4;
  if (count > (raw.size()+delta_page_size-1)/delta_page_size) return refuse(reason,"invalid delta page count");
  uint64_t previous = UINT64_MAX;
  for (size_t i = 0; i < count; ++i) {
    if (patch.size()-at < 8) return refuse(reason,"truncated delta page");
    const auto page = get(patch,at,4), n = get(patch,at+4,4); at += 8;
    const auto offset = page*delta_page_size;
    if ((i && page <= previous) || offset >= raw.size() || n != std::min<uint64_t>(delta_page_size,raw.size()-offset) || n > patch.size()-at)
      return refuse(reason,"invalid delta page");
    std::copy_n(patch.begin()+at,n,raw.begin()+offset); at += n; previous = page;
  }
  if (at != patch.size()) return refuse(reason,"oversized delta patch");
  std::vector<uint8_t> result(patch.begin()+4,patch.begin()+4+prefix);
  result.insert(result.end(),raw.begin(),raw.end());
  // Target checksum, identity, table and host length are validated before publication/load.
  std::vector<uint8_t> checked;
  if (!inspect(result,id,spans,state,sizes,checked,reason)) return false;
  target = std::move(result); if (pages) *pages = count; return true;
}

bool read_file(FILE* f, const std::vector<Span>& spans, std::vector<uint8_t>& file, std::string& reason) {
  reason.clear(); file.clear();
  uint64_t raw = 0;
  if (!layout(spans, raw, reason)) return false;
  if (!f) return refuse(reason, "cannot open snapshot");
  const size_t limit = static_cast<size_t>(header_size + table_entry_size * spans.size() + max_host_size + raw);
  uint8_t block[65536];
  for (;;) {
    const auto n = std::fread(block, 1, std::min(sizeof block, limit + 1 - file.size()), f);
    file.insert(file.end(), block, block+n);
    if (file.size() > limit) { file.clear(); return refuse(reason, "oversized file"); }
    if (std::ferror(f)) { file.clear(); return refuse(reason, "snapshot read failed"); }
    if (std::feof(f)) return true;
  }
}
bool write_file(FILE* f, const std::vector<uint8_t>& file, std::string& reason) {
  reason.clear();
  if (!f || std::fwrite(file.data(), 1, file.size(), f) != file.size() || std::fflush(f))
    return refuse(reason, "snapshot write failed");
  return true;
}
bool library_identity(const std::filesystem::path& path, Identity& id, std::string& reason) {
  reason.clear();
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) return refuse(reason, "cannot open game library");
  const auto end = in.tellg();
  if (end <= 0 || end > 256 * 1024 * 1024) return refuse(reason, "invalid game library size");
  std::vector<uint8_t> bytes(static_cast<size_t>(end));
  in.seekg(0);
  if (!in.read(reinterpret_cast<char*>(bytes.data()), bytes.size()) || in.peek() != std::char_traits<char>::eof())
    return refuse(reason, "game library read failed or size changed");
  id = {bytes.size(), digest(bytes.data(), bytes.size())};
  return true;
}
bool dump(FILE* f, const std::vector<Span>& spans, std::string& reason) {
  reason.clear();
  uint64_t raw = 0;
  if (!layout(spans, raw, reason)) return false;
  std::vector<uint8_t> header{'M','U','H','D'};
  put(header, 1, 4); put(header, spans.size(), 4); put(header, raw, 8); table(header, spans);
  if (!write_file(f, header, reason)) return false;
  for (const auto& s : spans)
    if (std::fwrite(s.address, 1, s.size, f) != s.size) return refuse(reason, "dump write failed");
  return std::fflush(f) == 0 || refuse(reason, "dump flush failed");
}
} // namespace handoff_state
