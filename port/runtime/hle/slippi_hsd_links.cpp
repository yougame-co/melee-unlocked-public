// SPDX-License-Identifier: GPL-2.0-or-later
// Slippi game-file asset links: port of the resolve path of Slippi's Rust crate slippi-hsd
// (archive.rs, link.rs, hash.rs). Ordering rules follow it exactly so the resolved file is the same
// bytes Slippi Dolphin serves: extents in order with their alignment phase, relocation table sorted,
// string table rebuilt roots first then references, copied assets appended in file order.
#include "slippi_hsd_links.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <set>

namespace slippi::hsd {

const char kLinkPrefix[] = "slp:";

namespace {

uint32_t be32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
void put_be32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
void append_be32(std::vector<uint8_t>& out, uint32_t v) {
  uint8_t b[4];
  put_be32(b, v);
  out.insert(out.end(), b, b + 4);
}
std::string hex32(uint64_t v) {
  char b[24];
  std::snprintf(b, sizeof b, "%llx", (unsigned long long)v);
  return b;
}
std::string dec(uint64_t v) { return std::to_string(v); }

bool fail(Error* error, ErrorKind kind, std::string message) {
  if (error) { error->kind = kind; error->message = std::move(message); }
  return false;
}
bool truncated(Error* error, uint64_t needed, uint64_t have) {
  return fail(error, ErrorKind::Truncated, "archive is truncated: needed " + dec(needed) + " bytes, have " + dec(have));
}
bool pointer_out_of_range(Error* error, uint32_t field, uint32_t target) {
  return fail(error, ErrorKind::PointerOutOfRange, "pointer at data offset 0x" + hex32(field) + " targets 0x" +
                                                       hex32(target) + ", outside the data section");
}
bool straddles(Error* error, uint32_t field) {
  return fail(error, ErrorKind::FieldStraddlesBoundary,
              "field at data offset 0x" + hex32(field) + " straddles a struct boundary");
}

// ---- SHA-1 (incremental) ----
uint32_t rotl(uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }
class Sha1 {
 public:
  void update(const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    total_ += n;
    if (used_) {
      const size_t take = std::min(sizeof buf_ - used_, n);
      std::memcpy(buf_ + used_, p, take);
      used_ += take; p += take; n -= take;
      if (used_ < sizeof buf_) return;
      block(buf_);
      used_ = 0;
    }
    for (; n >= 64; p += 64, n -= 64) block(p);
    if (n) { std::memcpy(buf_, p, n); used_ = n; }
  }
  Digest finish() {
    const uint64_t bits = total_ * 8;
    const uint8_t one = 0x80, zero = 0;
    update(&one, 1);
    while (used_ != 56) update(&zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    update(len, 8);
    Digest d;
    for (int i = 0; i < 5; ++i) put_be32(d.data() + 4 * i, h_[i]);
    return d;
  }

 private:
  void block(const uint8_t* m) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) w[i] = be32(m + 4 * i);
    for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
    for (int i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
      else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
      else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
      const uint32_t t = rotl(a, 5) + f + e + k + w[i];
      e = d; d = c; c = rotl(b, 30); b = a; a = t;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e;
  }
  uint32_t h_[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
  uint8_t buf_[64] = {};
  size_t used_ = 0;
  uint64_t total_ = 0;
};

// Rust's String::from_utf8_lossy: each maximal invalid subsequence becomes one U+FFFD.
std::string utf8_lossy(const uint8_t* p, size_t n) {
  std::string out;
  out.reserve(n);
  size_t i = 0;
  while (i < n) {
    const uint8_t c = p[i];
    if (c < 0x80) { out.push_back((char)c); ++i; continue; }
    size_t need = 0;
    uint8_t lo = 0x80, hi = 0xBF;
    if (c >= 0xC2 && c <= 0xDF) need = 1;
    else if (c >= 0xE0 && c <= 0xEF) { need = 2; if (c == 0xE0) lo = 0xA0; if (c == 0xED) hi = 0x9F; }
    else if (c >= 0xF0 && c <= 0xF4) { need = 3; if (c == 0xF0) lo = 0x90; if (c == 0xF4) hi = 0x8F; }
    size_t j = 1;
    bool ok = need > 0;
    for (; ok && j <= need; ++j) {
      if (i + j >= n) { ok = false; break; }
      const uint8_t d = p[i + j];
      if (d < (j == 1 ? lo : 0x80) || d > (j == 1 ? hi : 0xBF)) { ok = false; break; }
    }
    if (ok) { out.append(reinterpret_cast<const char*>(p + i), need + 1); i += need + 1; }
    else { out += "\xEF\xBF\xBD"; i += j; }
  }
  return out;
}

// A NUL-terminated name at `at` in a string table (to the table's end when unterminated).
bool read_cstr(const uint8_t* table, size_t size, uint32_t at, std::string* out, Error* error) {
  if (size < at) return truncated(error, at, size);
  size_t end = at;
  while (end < size && table[end]) ++end;
  *out = utf8_lossy(table + at, end - at);
  return true;
}

size_t trimmed_len(const std::vector<uint8_t>& data) {
  size_t end = data.size();
  while (end && !data[end - 1]) --end;
  return end;
}

std::vector<Pointer> sorted_by_field(std::vector<Pointer> pointers) {
  std::stable_sort(pointers.begin(), pointers.end(), [](const Pointer& x, const Pointer& y) { return x.field < y.field; });
  return pointers;
}
bool contains(const std::vector<uint32_t>& list, uint32_t v) { return std::find(list.begin(), list.end(), v) != list.end(); }

const Digest& cycle_digest() {
  static const Digest d = [] { Sha1 s; s.update("cycle", 5); return s.finish(); }();
  return d;
}

// Hashes of every extent's reachable structure, memoized (hash.rs Hasher). `cut_extent` hashes as
// if its pointers in `cut` were removed (the view asset_sha builds). Iterative, with the same visit
// order and memo/cycle behaviour as the recursive original, so deep pointer chains cannot exhaust
// the stack of the thread that loads game files.
class Hasher {
 public:
  explicit Hasher(const Archive& archive, size_t cut_extent = SIZE_MAX, const std::vector<uint32_t>* cut = nullptr)
      : a_(archive), cut_extent_(cut_extent), cut_(cut), memo_(archive.extents.size()),
        has_(archive.extents.size(), 0), busy_(archive.extents.size(), 0) {}

  Digest extent(size_t idx) {
    if (idx >= a_.extents.size()) return cycle_digest();
    if (has_[idx]) return memo_[idx];
    if (busy_[idx]) return cycle_digest();
    struct Frame {
      size_t idx;
      std::vector<Pointer> pointers;
      size_t next;
      std::vector<std::pair<uint32_t, Digest>> children;
    };
    std::vector<Frame> stack;
    auto open = [&](size_t i) {
      busy_[i] = 1;
      std::vector<Pointer> pointers = a_.extents[i].pointers;
      if (i == cut_extent_ && cut_)
        pointers.erase(std::remove_if(pointers.begin(), pointers.end(),
                                      [&](const Pointer& p) { return contains(*cut_, p.field); }),
                       pointers.end());
      stack.push_back(Frame{i, sorted_by_field(std::move(pointers)), 0, {}});
    };
    open(idx);
    Digest result{};
    while (!stack.empty()) {
      Frame& f = stack.back();
      if (f.next < f.pointers.size()) {
        const Pointer p = f.pointers[f.next];
        if (p.target >= a_.extents.size()) { f.children.emplace_back(p.field, cycle_digest()); ++f.next; continue; }
        if (has_[p.target]) { f.children.emplace_back(p.field, memo_[p.target]); ++f.next; continue; }
        if (busy_[p.target]) { f.children.emplace_back(p.field, cycle_digest()); ++f.next; continue; }
        open(p.target);   // f is not used past this point: the push may move it
        continue;
      }
      const Digest h = combine(a_.extents[f.idx].data, f.children);
      busy_[f.idx] = 0;
      has_[f.idx] = 1;
      memo_[f.idx] = h;
      stack.pop_back();
      if (stack.empty()) {
        result = h;
      } else {
        Frame& parent = stack.back();
        parent.children.emplace_back(parent.pointers[parent.next].field, h);
        ++parent.next;
      }
    }
    return result;
  }

  Digest archive() {
    Sha1 sha;
    const uint8_t nul = 0;
    for (const Root& root : a_.roots) {
      sha.update(root.name.data(), root.name.size());
      sha.update(&nul, 1);
      const Digest h = extent(root.extent);
      sha.update(h.data(), h.size());
    }
    for (const ExternRef& r : a_.refs) {
      sha.update(r.name.data(), r.name.size());
      sha.update(&nul, 1);
      for (const auto& loc : r.locations) {
        const Digest h = extent(loc.first);
        sha.update(h.data(), h.size());
        uint8_t field[4];
        put_be32(field, loc.second);
        sha.update(field, 4);
      }
    }
    return sha.finish();
  }

 private:
  const Archive& a_;
  size_t cut_extent_;
  const std::vector<uint32_t>* cut_;
  std::vector<Digest> memo_;
  std::vector<char> has_, busy_;
};

// Structural hash of `extent` without the cut pointers of its first struct, from the hashes of
// every extent in the archive.
Digest cut_hash(const Archive& archive, const std::vector<Digest>& hashes, size_t extent, const std::vector<uint32_t>& cut) {
  if (cut.empty()) return hashes[extent];
  std::vector<Pointer> kept;
  for (const Pointer& p : archive.extents[extent].pointers)
    if (!contains(cut, p.field)) kept.push_back(p);
  kept = sorted_by_field(std::move(kept));
  std::vector<std::pair<uint32_t, Digest>> children;
  for (const Pointer& p : kept) children.emplace_back(p.field, p.target < hashes.size() ? hashes[p.target] : cycle_digest());
  return combine(archive.extents[extent].data, children);
}

// Rust's u32::from_str_radix / usize::from_str: an optional '+', then at least one digit.
bool parse_unsigned(const std::string& text, unsigned radix, uint64_t max, uint64_t* out) {
  size_t i = 0;
  if (text.empty()) return false;
  if (text[0] == '+') {
    if (text.size() == 1) return false;
    i = 1;
  }
  uint64_t v = 0;
  for (; i < text.size(); ++i) {
    const char c = text[i];
    unsigned d;
    if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
    else if (c >= 'a' && c <= 'z') d = (unsigned)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'Z') d = (unsigned)(c - 'A' + 10);
    else return false;
    if (d >= radix) return false;
    if (v > (max - d) / radix) return false;
    v = v * radix + d;
  }
  *out = v;
  return true;
}

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> parts;
  size_t start = 0;
  for (;;) {
    const size_t at = s.find(sep, start);
    if (at == std::string::npos) { parts.push_back(s.substr(start)); return parts; }
    parts.push_back(s.substr(start, at - start));
    start = at + 1;
  }
}

bool parse_hex(const std::string& symbol, const std::string& text, uint32_t* out, Error* error) {
  std::string digits = text;
  if (text.compare(0, 2, "0x") == 0 || text.compare(0, 2, "0X") == 0) digits = text.substr(2);
  uint64_t v = 0;
  if (!parse_unsigned(digits, 16, 0xFFFFFFFFull, &v))
    return fail(error, ErrorKind::BadLocator, "malformed link symbol " + symbol + ": bad number " + text);
  *out = (uint32_t)v;
  return true;
}

// The extent an anchor names in `archive`.
bool at_anchor(const Archive& archive, const Anchor& anchor, size_t* out, std::string* reason) {
  if (anchor.kind == Anchor::Kind::Offset) {
    const auto e = archive.extent_at_origin(anchor.offset - kHeaderSize);
    if (!e) { *reason = "no struct starts at file offset 0x" + hex32(anchor.offset); return false; }
    *out = *e;
    return true;
  }
  return follow(archive, anchor.root, anchor.hops, out, reason);
}

// The other region's copy of a disc file: menu and scene archives ship as a Japanese .dat and an
// American .usd with the same structure, and a mod usually rebuilds only the one it targets.
std::optional<std::string> regional_twin(const std::string& file) {
  const size_t dot = file.rfind('.');
  if (dot == std::string::npos) return std::nullopt;
  const std::string ext = file.substr(dot + 1);
  const char* other = ext == "usd" ? "dat" : ext == "dat" ? "usd" : nullptr;
  if (!other) return std::nullopt;
  return file.substr(0, dot) + "." + other;
}

// A disc file loaded for resolving.
struct Source {
  Archive archive;
  std::optional<std::vector<Digest>> hashes;   // every extent's hash, computed when an asset is searched for
};

// The disc files read while resolving, each read at most once.
struct Sources {
  const Load& load;
  std::map<std::string, std::pair<std::optional<Source>, std::string>> files;   // the source, or why not
  std::vector<std::string> read;   // files read successfully, in order

  Source* get(const std::string& file, std::string* reason) {
    auto it = files.find(file);
    if (it == files.end()) {
      std::pair<std::optional<Source>, std::string> entry;
      std::vector<uint8_t> bytes;
      std::string why;
      if (!load) {
        entry.second = "no disc reader was provided";
      } else if (!load(file, &bytes, &why)) {
        entry.second = why;
      } else {
        Source source;
        Error error;
        if (Archive::parse(bytes.data(), bytes.size(), &source.archive, &error)) {
          entry.first = std::move(source);
          read.push_back(file);
        } else {
          entry.second = error.message;
        }
      }
      it = files.emplace(file, std::move(entry)).first;
    }
    if (!it->second.first) { *reason = it->second.second; return nullptr; }
    return &*it->second.first;
  }
};

// Find the asset a locator records by its hash: at the fallback offset, else anywhere.
std::optional<size_t> find_by_hash(Source& src, const Locator& locator) {
  if (!locator.sha) return std::nullopt;
  const std::string& sha = *locator.sha;
  const Archive& archive = src.archive;
  if (locator.at) {
    const auto extent = archive.extent_at_origin(*locator.at - kHeaderSize);
    if (extent && asset_sha(archive, *extent, locator.cut) == sha) return extent;
  }
  if (!src.hashes) src.hashes = extent_hashes(archive);
  std::vector<uint8_t> prefix;
  for (size_t i = 0; i < sha.size() / 2; ++i) {
    uint64_t v = 0;
    prefix.push_back(parse_unsigned(sha.substr(2 * i, 2), 16, 0xFF, &v) ? (uint8_t)v : 0);
  }
  // Hashes memoized across the whole file can differ from a fresh one for an asset that loops back
  // into itself, so they only pick candidates.
  for (size_t i = 0; i < archive.extents.size(); ++i) {
    const Digest h = cut_hash(archive, *src.hashes, i, locator.cut);
    if (prefix.size() <= h.size() && std::equal(prefix.begin(), prefix.end(), h.begin()) &&
        asset_sha(archive, i, locator.cut) == sha)
      return i;
  }
  return std::nullopt;
}

struct Found {
  std::string file;
  size_t extent = 0;
  LinkStatus status = LinkStatus::Exact;
};

// Find the struct a locator names: by its anchor in the named file, which follows a mod that
// replaced the asset in place; else by its recorded hash in that file, then in the other region's
// copy of the file; else the other region's asset at the same path.
bool find_asset(Sources& sources, const Locator& locator, const std::string& symbol, std::vector<std::string>& warnings,
                Found* found, Error* error) {
  const std::string& file = locator.file;
  std::vector<std::string> searched;   // files searched by hash, for the error message
  std::string problem;
  std::optional<std::string> unreadable;
  std::string reason;
  if (Source* src = sources.get(file, &reason)) {
    std::string why;
    size_t extent = 0;
    if (at_anchor(src->archive, locator.anchor, &extent, &why)) {
      LinkStatus status = LinkStatus::Unchecked;
      if (locator.sha) {
        const std::string& expected = *locator.sha;
        const std::string actual = asset_sha(src->archive, extent, locator.cut);
        if (actual == expected) {
          status = LinkStatus::Exact;
        } else if (locator.strict) {
          return fail(error, ErrorKind::HashMismatch, "linked asset " + symbol + " does not match its recorded hash (expected " +
                                                          expected + ", found " + actual + ")");
        } else {
          warnings.push_back(symbol + ": the asset on the disc differs from the recorded one (" + expected + " vs " + actual +
                             "); using the disc's version");
          status = LinkStatus::Modified;
        }
      }
      *found = {file, extent, status};
      return true;
    }
    if (const auto e = find_by_hash(*src, locator)) {
      const uint32_t offset = src->archive.extents[*e].origin + kHeaderSize;
      warnings.push_back(symbol + ": " + why + "; found the recorded asset at 0x" + hex32(offset) + " in " + file);
      *found = {file, *e, LinkStatus::Fallback};
      return true;
    }
    if (locator.sha) {
      searched.push_back(file);
    } else if (locator.at) {
      if (const auto e = src->archive.extent_at_origin(*locator.at - kHeaderSize)) {
        // Without a hash the fallback offset cannot be checked, only trusted.
        warnings.push_back(symbol + ": " + why + "; used the fallback offset");
        *found = {file, *e, LinkStatus::Fallback};
        return true;
      }
    }
    problem = why;
  } else {
    problem = file + " could not be read (" + reason + ")";
    unreadable = reason;
  }

  if (const auto twin = regional_twin(file)) {
    std::string twin_reason;
    if (Source* src = sources.get(*twin, &twin_reason)) {
      // An offset means nothing in another file, but a path names the same asset.
      std::optional<size_t> slot;
      if (locator.anchor.kind == Anchor::Kind::Path) {
        size_t e = 0;
        std::string ignored;
        if (at_anchor(src->archive, locator.anchor, &e, &ignored)) slot = e;
      }
      const bool slot_is_recorded = slot && locator.sha && asset_sha(src->archive, *slot, locator.cut) == *locator.sha;
      const std::optional<size_t> recorded = slot_is_recorded ? slot : find_by_hash(*src, locator);
      if (recorded) {
        warnings.push_back(symbol + ": " + problem + "; found the recorded asset in " + *twin);
        *found = {*twin, *recorded, LinkStatus::Fallback};
        return true;
      }
      if (slot && !locator.strict) {
        warnings.push_back(symbol + ": " + problem + "; used the same asset of " + *twin +
                           ", which differs from the recorded one");
        *found = {*twin, *slot, LinkStatus::Regional};
        return true;
      }
      if (locator.sha) searched.push_back(*twin);
    }
  }

  if (unreadable) return fail(error, ErrorKind::Source, "could not read disc file " + file + ": " + *unreadable);
  if (locator.anchor.kind == Anchor::Kind::Offset)
    return fail(error, ErrorKind::NotAStruct,
                "disc file " + file + " has no struct starting at file offset 0x" + hex32(locator.anchor.offset));
  std::string why = problem;
  if (!searched.empty()) {
    why += "; the recorded asset is not in ";
    for (size_t i = 0; i < searched.size(); ++i) why += (i ? " or " : "") + searched[i];
  }
  return fail(error, ErrorKind::BadPath, "link " + symbol + " does not resolve in the disc file: " + why);
}

std::vector<std::string> sorted_ref_names(const Archive& a) {
  std::vector<std::string> names;
  for (const ExternRef& r : a.refs) names.push_back(r.name);
  std::sort(names.begin(), names.end());
  return names;
}

std::string links_differ(const std::vector<std::string>& na, const std::vector<std::string>& nb) {
  size_t only_a = 0, only_b = 0;
  for (const auto& n : na) only_a += std::find(nb.begin(), nb.end(), n) == nb.end();
  for (const auto& n : nb) only_b += std::find(na.begin(), na.end(), n) == na.end();
  return "links differ (" + dec(only_a) + " only in the first, " + dec(only_b) + " only in the second)";
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Archive

bool Archive::parse(const uint8_t* bytes, size_t size, Archive* out, Error* error) {
  if (size < kHeaderSize) return truncated(error, kHeaderSize, size);
  const uint32_t file_size = be32(bytes);
  const uint32_t data_size = be32(bytes + 4);
  const uint64_t reloc_count = be32(bytes + 8);
  const uint64_t root_count = be32(bytes + 12);
  const uint64_t ref_count = be32(bytes + 16);
  Archive a;
  std::memcpy(a.version.data(), bytes + 0x14, 4);
  std::memcpy(a.header_tail.data(), bytes + 0x18, 8);

  if (file_size != size)
    return fail(error, ErrorKind::SizeMismatch,
                "header says the file is " + dec(file_size) + " bytes but it is " + dec(size));

  const uint64_t data_start = kHeaderSize;
  const uint64_t data_end = data_start + data_size;
  const uint64_t reloc_off = data_end;
  const uint64_t roots_off = reloc_off + reloc_count * 4;
  const uint64_t refs_off = roots_off + root_count * 8;
  const uint64_t strings_off = refs_off + ref_count * 8;
  if (size < strings_off) return truncated(error, strings_off, size);

  const uint8_t* data = bytes + data_start;
  const size_t data_len = data_size;
  const uint8_t* strings = bytes + strings_off;
  const size_t strings_len = size - (size_t)strings_off;

  // Relocation table: every pointer field, and the extent starts they imply.
  std::vector<std::pair<uint32_t, uint32_t>> fields;
  fields.reserve((size_t)reloc_count);
  std::set<uint32_t> boundaries{0, data_size};
  for (uint64_t i = 0; i < reloc_count; ++i) {
    const uint32_t field = be32(bytes + reloc_off + i * 4);
    if ((uint64_t)field + 4 > data_len) return pointer_out_of_range(error, field, 0);
    const uint32_t target = be32(data + field);
    if (target > data_size) return pointer_out_of_range(error, field, target);
    boundaries.insert(target);
    fields.emplace_back(field, target);
  }

  // Roots.
  std::vector<uint32_t> root_offsets, root_targets;
  std::vector<std::string> root_names;
  for (uint64_t i = 0; i < root_count; ++i) {
    const uint32_t target = be32(bytes + roots_off + i * 8);
    const uint32_t str_off = be32(bytes + roots_off + i * 8 + 4);
    std::string name;
    if (!read_cstr(strings, strings_len, str_off, &name, error)) return false;
    if (target > data_size)
      return fail(error, ErrorKind::RootOutOfRange,
                  "root " + name + " targets 0x" + hex32(target) + ", outside the data section");
    boundaries.insert(target);
    root_offsets.push_back(str_off);
    root_names.push_back(std::move(name));
    root_targets.push_back(target);
  }

  // External references and their chains.
  std::vector<uint32_t> ref_offsets;
  std::vector<std::string> ref_names;
  std::vector<std::vector<uint32_t>> ref_chains;
  for (uint64_t i = 0; i < ref_count; ++i) {
    const uint32_t head = be32(bytes + refs_off + i * 8);
    const uint32_t str_off = be32(bytes + refs_off + i * 8 + 4);
    std::string name;
    if (!read_cstr(strings, strings_len, str_off, &name, error)) return false;
    std::vector<uint32_t> chain;
    uint32_t at = head;
    for (;;) {
      if ((uint64_t)at + 4 > data_len || chain.size() > data_len / 4)
        return fail(error, ErrorKind::BadChain,
                    "external reference " + name + " has a malformed chain at data offset 0x" + hex32(at));
      chain.push_back(at);
      const uint32_t next = be32(data + at);
      if (next == kChainEnd) break;
      at = next;
    }
    ref_offsets.push_back(str_off);
    ref_names.push_back(std::move(name));
    ref_chains.push_back(std::move(chain));
  }

  // Cut the data section into extents. A pointer or root may target the very end of the data
  // section (an empty struct): it is kept as a zero-length extent so the pointer survives.
  bool end_targeted = false;
  for (const auto& f : fields) end_targeted |= f.second == data_size;
  for (uint32_t t : root_targets) end_targeted |= t == data_size;
  std::vector<uint32_t> starts;
  for (uint32_t b : boundaries)
    if (b < data_size || (end_targeted && b == data_size)) starts.push_back(b);
  a.extents.reserve(starts.size());
  for (size_t i = 0; i < starts.size(); ++i) {
    const uint32_t start = starts[i];
    const uint32_t end = i + 1 < starts.size() ? starts[i + 1] : data_size;
    Extent e;
    e.data.assign(data + start, data + end);
    e.phase = start % kAlign;
    e.origin = start;
    a.extents.push_back(std::move(e));
  }
  auto index_of = [&](uint32_t target) -> std::optional<size_t> {
    const auto it = std::lower_bound(starts.begin(), starts.end(), target);
    if (it == starts.end() || *it != target) return std::nullopt;
    return (size_t)(it - starts.begin());
  };
  // The extent containing a field, and the field's offset in it; the field must fit inside it.
  auto locate_field = [&](uint32_t field, size_t* idx, uint32_t* within) {
    const auto it = std::upper_bound(starts.begin(), starts.end(), field);
    if (it == starts.begin()) return straddles(error, field);
    *idx = (size_t)(it - starts.begin()) - 1;
    *within = field - starts[*idx];
    if ((uint64_t)*within + 4 > a.extents[*idx].data.size()) return straddles(error, field);
    return true;
  };

  for (const auto& f : fields) {
    size_t idx = 0;
    uint32_t within = 0;
    if (!locate_field(f.first, &idx, &within)) return false;
    const auto target = index_of(f.second);
    if (!target) return pointer_out_of_range(error, f.first, f.second);
    std::memset(a.extents[idx].data.data() + within, 0, 4);
    a.extents[idx].pointers.push_back(Pointer{within, *target});
  }

  for (size_t i = 0; i < root_names.size(); ++i) a.roots.push_back(Root{root_names[i], *index_of(root_targets[i])});

  for (size_t i = 0; i < ref_names.size(); ++i) {
    ExternRef r;
    r.name = ref_names[i];
    r.locations.reserve(ref_chains[i].size());
    for (uint32_t at : ref_chains[i]) {
      size_t idx = 0;
      uint32_t within = 0;
      if (!locate_field(at, &idx, &within)) return false;
      std::memset(a.extents[idx].data.data() + within, 0, 4);
      r.locations.emplace_back(idx, within);
    }
    a.refs.push_back(std::move(r));
  }

  StringTable table;
  table.bytes.assign(strings, strings + strings_len);
  table.root_offsets = std::move(root_offsets);
  table.ref_offsets = std::move(ref_offsets);
  a.strings = std::move(table);
  *out = std::move(a);
  return true;
}

std::vector<uint32_t> Archive::layout() const {
  std::vector<uint32_t> offsets;
  offsets.reserve(extents.size());
  uint32_t cursor = 0;
  for (const Extent& e : extents) {
    cursor += (e.phase + kAlign - cursor % kAlign) % kAlign;
    offsets.push_back(cursor);
    cursor += e.len();
  }
  return offsets;
}

bool Archive::serialize(std::vector<uint8_t>* out, Error* error) const {
  // Lay out extents: in order, each at the next offset with its recorded alignment phase.
  const std::vector<uint32_t> offsets = layout();
  const uint32_t data_size = extents.empty() ? 0 : offsets.back() + extents.back().len();

  // Data section with pointers and chains filled in.
  std::vector<uint8_t> data(data_size);
  std::vector<uint32_t> relocs;
  for (size_t i = 0; i < extents.size(); ++i) {
    const Extent& e = extents[i];
    const uint32_t base = offsets[i];
    if (!e.data.empty()) std::memcpy(data.data() + base, e.data.data(), e.data.size());
    for (const Pointer& p : e.pointers) {
      const uint64_t at = (uint64_t)base + p.field;
      if (p.target >= extents.size() || at + 4 > data.size()) return pointer_out_of_range(error, p.field, 0);
      put_be32(data.data() + at, offsets[p.target]);
      relocs.push_back((uint32_t)at);
    }
  }
  std::sort(relocs.begin(), relocs.end());

  std::vector<uint32_t> ref_heads;
  ref_heads.reserve(refs.size());
  for (const ExternRef& r : refs) {
    if (r.locations.empty()) return fail(error, ErrorKind::EmptyRef, "external reference " + r.name + " has no locations");
    for (const auto& loc : r.locations)
      if (loc.first >= extents.size()) return fail(error, ErrorKind::NoSuchExtent, "extent index " + dec(loc.first) + " is out of range");
    auto place = [&](const std::pair<size_t, uint32_t>& loc) { return offsets[loc.first] + loc.second; };
    for (size_t n = 0; n < r.locations.size(); ++n) {
      const uint32_t value = n + 1 < r.locations.size() ? place(r.locations[n + 1]) : kChainEnd;
      const uint64_t at = place(r.locations[n]);
      if (at + 4 > data.size()) return pointer_out_of_range(error, r.locations[n].second, 0);
      put_be32(data.data() + at, value);
    }
    ref_heads.push_back(place(r.locations[0]));
  }

  // String table: reuse the original when the symbols are unchanged.
  const StringTable* reuse = nullptr;
  if (strings && strings->root_offsets.size() == roots.size() && strings->ref_offsets.size() == refs.size()) {
    reuse = &*strings;
    auto same_name = [&](uint32_t offset, const std::string& name) {
      std::string s;
      return read_cstr(strings->bytes.data(), strings->bytes.size(), offset, &s, nullptr) && s == name;
    };
    for (size_t i = 0; reuse && i < roots.size(); ++i)
      if (!same_name(strings->root_offsets[i], roots[i].name)) reuse = nullptr;
    for (size_t i = 0; reuse && i < refs.size(); ++i)
      if (!same_name(strings->ref_offsets[i], refs[i].name)) reuse = nullptr;
  }
  std::vector<uint8_t> table;
  std::vector<uint32_t> root_str, ref_str;
  if (reuse) {
    table = reuse->bytes;
    root_str = reuse->root_offsets;
    ref_str = reuse->ref_offsets;
  } else {
    for (const Root& root : roots) {
      root_str.push_back((uint32_t)table.size());
      table.insert(table.end(), root.name.begin(), root.name.end());
      table.push_back(0);
    }
    for (const ExternRef& r : refs) {
      ref_str.push_back((uint32_t)table.size());
      table.insert(table.end(), r.name.begin(), r.name.end());
      table.push_back(0);
    }
  }

  const size_t file_size = kHeaderSize + data.size() + relocs.size() * 4 + (roots.size() + refs.size()) * 8 + table.size();
  std::vector<uint8_t> bytes;
  bytes.reserve(file_size);
  append_be32(bytes, (uint32_t)file_size);
  append_be32(bytes, data_size);
  append_be32(bytes, (uint32_t)relocs.size());
  append_be32(bytes, (uint32_t)roots.size());
  append_be32(bytes, (uint32_t)refs.size());
  bytes.insert(bytes.end(), version.begin(), version.end());
  bytes.insert(bytes.end(), header_tail.begin(), header_tail.end());
  bytes.insert(bytes.end(), data.begin(), data.end());
  for (uint32_t r : relocs) append_be32(bytes, r);
  for (size_t i = 0; i < roots.size(); ++i) {
    if (roots[i].extent >= extents.size()) return fail(error, ErrorKind::NoSuchExtent, "extent index " + dec(roots[i].extent) + " is out of range");
    append_be32(bytes, offsets[roots[i].extent]);
    append_be32(bytes, root_str[i]);
  }
  for (size_t i = 0; i < refs.size(); ++i) {
    append_be32(bytes, ref_heads[i]);
    append_be32(bytes, ref_str[i]);
  }
  bytes.insert(bytes.end(), table.begin(), table.end());
  *out = std::move(bytes);
  return true;
}

std::optional<size_t> Archive::extent_at_origin(uint32_t data_offset) const {
  for (size_t i = 0; i < extents.size(); ++i)
    if (extents[i].origin == data_offset) return i;
  return std::nullopt;
}

std::vector<size_t> Archive::reachable(const std::vector<size_t>& starts,
                                       const std::function<bool(size_t, const Pointer&)>& cut) const {
  std::vector<char> seen(extents.size(), 0);
  std::vector<size_t> order;
  std::deque<size_t> queue;
  for (size_t s : starts) {
    if (s < extents.size() && !seen[s]) { seen[s] = 1; queue.push_back(s); }
  }
  while (!queue.empty()) {
    const size_t idx = queue.front();
    queue.pop_front();
    order.push_back(idx);
    for (const Pointer& p : extents[idx].pointers) {
      if ((cut && cut(idx, p)) || p.target >= extents.size() || seen[p.target]) continue;
      seen[p.target] = 1;
      queue.push_back(p.target);
    }
  }
  return order;
}

std::vector<size_t> Archive::reachable_from_roots() const {
  std::vector<size_t> starts;
  for (const Root& r : roots) starts.push_back(r.extent);
  return reachable(starts);
}

bool Archive::remove_extents(const std::vector<size_t>& remove, Error* error) {
  std::vector<char> gone(extents.size(), 0);
  for (size_t i : remove) {
    if (i >= extents.size()) return fail(error, ErrorKind::NoSuchExtent, "extent index " + dec(i) + " is out of range");
    gone[i] = 1;
  }
  std::vector<size_t> new_index(extents.size(), SIZE_MAX);
  size_t next = 0;
  for (size_t i = 0; i < gone.size(); ++i)
    if (!gone[i]) new_index[i] = next++;
  for (const Root& root : roots)
    if (gone[root.extent])
      return fail(error, ErrorKind::CannotLinkRoot, "extent " + dec(root.extent) + " is a root and cannot be linked");
  for (const ExternRef& r : refs)
    for (const auto& loc : r.locations)
      if (gone[loc.first])
        return fail(error, ErrorKind::RefInsideLinkedSubtree,
                    "external reference " + r.name + " has a location inside the subtree being linked");
  for (size_t i = 0; i < extents.size(); ++i) {
    if (gone[i]) continue;
    for (const Pointer& p : extents[i].pointers)
      if (gone[p.target]) return pointer_out_of_range(error, p.field, extents[p.target].origin);
  }
  std::vector<Extent> kept;
  kept.reserve(next);
  for (size_t i = 0; i < extents.size(); ++i) {
    if (gone[i]) continue;
    Extent e = std::move(extents[i]);
    for (Pointer& p : e.pointers) p.target = new_index[p.target];
    kept.push_back(std::move(e));
  }
  extents = std::move(kept);
  for (Root& root : roots) root.extent = new_index[root.extent];
  for (ExternRef& r : refs)
    for (auto& loc : r.locations) loc.first = new_index[loc.first];
  return true;
}

// ---------------------------------------------------------------------------------------------
// Hashing

Digest combine(const std::vector<uint8_t>& data, const std::vector<std::pair<uint32_t, Digest>>& children) {
  Sha1 sha;
  sha.update(data.data(), trimmed_len(data));
  for (const auto& child : children) {
    uint8_t field[4];
    put_be32(field, child.first);
    sha.update(field, 4);
    sha.update(child.second.data(), child.second.size());
  }
  return sha.finish();
}

Digest extent_hash(const Archive& archive, size_t extent) { return Hasher(archive).extent(extent); }

std::vector<Digest> extent_hashes(const Archive& archive) {
  Hasher hasher(archive);
  std::vector<Digest> hashes;
  hashes.reserve(archive.extents.size());
  for (size_t i = 0; i < archive.extents.size(); ++i) hashes.push_back(hasher.extent(i));
  return hashes;
}

Digest archive_hash(const Archive& archive) { return Hasher(archive).archive(); }

bool same_structure(const Archive& a, const Archive& b) { return archive_hash(a) == archive_hash(b); }

std::string hex(const uint8_t* bytes, size_t size) {
  static const char digits[] = "0123456789abcdef";
  std::string s;
  s.reserve(size * 2);
  for (size_t i = 0; i < size; ++i) { s.push_back(digits[bytes[i] >> 4]); s.push_back(digits[bytes[i] & 15]); }
  return s;
}

// ---------------------------------------------------------------------------------------------
// Links

std::string Anchor::text() const {
  if (kind == Kind::Offset) return "0x" + hex32(offset);
  std::string t = root + "/";
  size_t i = 0;
  while (i < hops.size()) {
    size_t n = 1;
    while (i + n < hops.size() && hops[i + n] == hops[i]) ++n;
    if (i > 0) t.push_back('/');
    t += hex32(hops[i]);
    if (n > 1) t += "*" + dec(n);
    i += n;
  }
  return t;
}

bool Locator::is_link(const std::string& symbol) { return symbol.compare(0, 4, kLinkPrefix) == 0; }

bool Locator::parse(const std::string& symbol, Locator* out, Error* error) {
  auto bad = [&](const std::string& reason) {
    return fail(error, ErrorKind::BadLocator, "malformed link symbol " + symbol + ": " + reason);
  };
  if (!is_link(symbol)) return bad("missing slp: prefix");
  const std::vector<std::string> parts = split(symbol.substr(4), ':');
  if (parts[0].empty()) return bad("missing file name");
  if (parts.size() < 2) return bad("missing anchor");
  Locator l;
  l.file = parts[0];
  const std::string& anchor_text = parts[1];
  const size_t slash = anchor_text.find('/');
  if (slash != std::string::npos) {
    const std::string root = anchor_text.substr(0, slash);
    if (root.empty()) return bad("missing root symbol");
    l.anchor.kind = Anchor::Kind::Path;
    l.anchor.root = root;
    for (const std::string& hop : split(anchor_text.substr(slash + 1), '/')) {
      if (hop.empty()) continue;
      std::string field_text = hop;
      uint64_t count = 1;
      const size_t star = hop.find('*');
      if (star != std::string::npos) {
        field_text = hop.substr(0, star);
        const std::string n = hop.substr(star + 1);
        // A count past what any archive could hold would only exhaust memory.
        if (!parse_unsigned(n, 10, UINT64_MAX, &count) || count > (1u << 20)) return bad("bad repeat count " + n);
      }
      uint32_t field = 0;
      if (!parse_hex(symbol, field_text, &field, error)) return false;
      l.anchor.hops.insert(l.anchor.hops.end(), (size_t)count, field);
    }
  } else {
    uint32_t offset = 0;
    if (!parse_hex(symbol, anchor_text, &offset, error)) return false;
    if (offset < kHeaderSize) return bad("offset is inside the header");
    l.anchor.kind = Anchor::Kind::Offset;
    l.anchor.offset = offset;
  }
  for (size_t i = 2; i < parts.size(); ++i) {
    const std::string& part = parts[i];
    const size_t eq = part.find('=');
    const std::string key = eq == std::string::npos ? std::string() : part.substr(0, eq);
    const std::string value = eq == std::string::npos ? std::string() : part.substr(eq + 1);
    if (eq != std::string::npos && key == "at") {
      uint32_t offset = 0;
      if (!parse_hex(symbol, value, &offset, error)) return false;
      if (offset < kHeaderSize) return bad("at offset is inside the header");
      l.at = offset;
    } else if (eq != std::string::npos && key == "cut") {
      for (const std::string& item : split(value, ',')) {
        uint32_t field = 0;
        if (!parse_hex(symbol, item, &field, error)) return false;
        l.cut.push_back(field);
      }
    } else if (eq != std::string::npos && key == "sha" && value.size() == 8 &&
               std::all_of(value.begin(), value.end(), [](char c) { return std::isxdigit((unsigned char)c) != 0; })) {
      std::string lower = value;
      for (char& c : lower) if (c >= 'A' && c <= 'F') c = (char)(c - 'A' + 'a');
      l.sha = lower;
    } else if (eq != std::string::npos && key == "strict" && value == "1") {
      l.strict = true;
    } else {
      return bad("unknown option " + part);
    }
  }
  *out = std::move(l);
  return true;
}

std::string Locator::symbol() const {
  std::string s = std::string(kLinkPrefix) + file + ":" + anchor.text();
  if (at) s += ":at=0x" + hex32(*at);
  if (!cut.empty()) {
    s += ":cut=";
    for (size_t i = 0; i < cut.size(); ++i) s += (i ? "," : "") + hex32(cut[i]);
  }
  if (sha) s += ":sha=" + *sha;
  if (strict) s += ":strict=1";
  return s;
}

const char* link_status_name(LinkStatus status) {
  switch (status) {
    case LinkStatus::Exact: return "exact";
    case LinkStatus::Modified: return "modified";
    case LinkStatus::Fallback: return "fallback";
    case LinkStatus::Regional: return "regional";
    case LinkStatus::Unchecked: return "unchecked";
  }
  return "?";
}

std::string asset_sha(const Archive& archive, size_t extent, const std::vector<uint32_t>& cut) {
  const Digest d = cut.empty() ? Hasher(archive).extent(extent) : Hasher(archive, extent, &cut).extent(extent);
  return hex(d.data(), d.size()).substr(0, 8);
}

bool follow(const Archive& archive, const std::string& root, const std::vector<uint32_t>& hops, size_t* out,
            std::string* reason) {
  const auto r = std::find_if(archive.roots.begin(), archive.roots.end(), [&](const Root& x) { return x.name == root; });
  if (r == archive.roots.end()) { *reason = "no root symbol " + root; return false; }
  size_t cur = r->extent;
  for (size_t n = 0; n < hops.size(); ++n) {
    const auto& pointers = archive.extents[cur].pointers;
    const auto p = std::find_if(pointers.begin(), pointers.end(), [&](const Pointer& x) { return x.field == hops[n]; });
    if (p == pointers.end()) {
      *reason = "hop " + dec(n) + " (field 0x" + hex32(hops[n]) + ") is not a pointer";
      return false;
    }
    cur = p->target;
  }
  *out = cur;
  return true;
}

Locator locate(const Archive& archive, const std::string& file, size_t extent, const std::vector<uint32_t>& cut) {
  const uint32_t offset = archive.extents[extent].origin + kHeaderSize;
  // Breadth-first search from every root, remembering how each extent was reached.
  const size_t n = archive.extents.size();
  std::vector<std::optional<std::pair<size_t, uint32_t>>> parent(n);
  std::vector<std::optional<size_t>> root_of(n);
  std::deque<size_t> queue;
  for (size_t ri = 0; ri < archive.roots.size(); ++ri) {
    const size_t e = archive.roots[ri].extent;
    if (!root_of[e]) { root_of[e] = ri; queue.push_back(e); }
  }
  while (!queue.empty()) {
    const size_t idx = queue.front();
    queue.pop_front();
    if (idx == extent) break;
    for (const Pointer& p : sorted_by_field(archive.extents[idx].pointers)) {
      if (!root_of[p.target]) {
        root_of[p.target] = root_of[idx];
        parent[p.target] = std::make_pair(idx, p.field);
        queue.push_back(p.target);
      }
    }
  }
  Locator l;
  l.file = file;
  if (root_of[extent]) {
    l.anchor.kind = Anchor::Kind::Path;
    l.anchor.root = archive.roots[*root_of[extent]].name;
    size_t cur = extent;
    while (parent[cur]) {
      l.anchor.hops.push_back(parent[cur]->second);
      cur = parent[cur]->first;
    }
    std::reverse(l.anchor.hops.begin(), l.anchor.hops.end());
  } else {
    l.anchor.kind = Anchor::Kind::Offset;
    l.anchor.offset = offset;
  }
  l.at = offset;
  l.cut = cut;
  l.sha = asset_sha(archive, extent, cut);
  return l;
}

bool link(Archive& archive, size_t extent, const std::string& symbol, Error* error) {
  if (extent >= archive.extents.size())
    return fail(error, ErrorKind::NoSuchExtent, "extent index " + dec(extent) + " is out of range");
  for (const Root& r : archive.roots)
    if (r.extent == extent)
      return fail(error, ErrorKind::CannotLinkRoot, "extent " + dec(extent) + " is a root and cannot be linked");
  const std::vector<size_t> before = archive.reachable_from_roots();

  std::vector<std::pair<size_t, uint32_t>> locations;
  for (size_t i = 0; i < archive.extents.size(); ++i) {
    auto& pointers = archive.extents[i].pointers;
    std::vector<Pointer> kept;
    for (const Pointer& p : pointers) {
      if (p.target == extent) locations.emplace_back(i, p.field);
      else kept.push_back(p);
    }
    pointers = std::move(kept);
  }
  if (locations.empty())
    return fail(error, ErrorKind::Unreferenced, "extent " + dec(extent) + " is not referenced by any pointer");
  const auto existing = std::find_if(archive.refs.begin(), archive.refs.end(), [&](const ExternRef& r) { return r.name == symbol; });
  if (existing != archive.refs.end()) existing->locations.insert(existing->locations.end(), locations.begin(), locations.end());
  else archive.refs.push_back(ExternRef{symbol, locations});

  std::vector<char> still(archive.extents.size(), 0);
  for (size_t i : archive.reachable_from_roots()) still[i] = 1;
  std::vector<size_t> removed;
  for (size_t i : before)
    if (!still[i]) removed.push_back(i);
  if (!archive.remove_extents(removed, error)) return false;
  archive.invalidate_strings();
  return true;
}

bool resolve(const Archive& archive, const Load& load, Resolution* out, Error* error) {
  Resolution res;
  Archive result = archive;
  result.refs.clear();
  Sources sources{load, {}, {}};
  bool resolved_any = false;

  for (const ExternRef& r : archive.refs) {
    if (!Locator::is_link(r.name)) {
      result.refs.push_back(r);
      continue;
    }
    Locator locator;
    if (!Locator::parse(r.name, &locator, error)) return false;
    Found found;
    if (!find_asset(sources, locator, r.name, res.warnings, &found, error)) return false;
    std::string ignored;
    const Archive& src = sources.get(found.file, &ignored)->archive;   // the asset was found in it
    const size_t root = found.extent;

    const std::vector<size_t> closure =
        src.reachable({root}, [&](size_t e, const Pointer& p) { return e == root && contains(locator.cut, p.field); });
    std::vector<size_t> ordered = closure;
    std::stable_sort(ordered.begin(), ordered.end(),
                     [&](size_t x, size_t y) { return src.extents[x].origin < src.extents[y].origin; });
    std::map<size_t, size_t> new_index;
    for (size_t i : ordered) {
      new_index[i] = result.extents.size();
      Extent e;
      e.data = src.extents[i].data;
      e.phase = src.extents[i].phase;
      e.origin = src.extents[i].origin;
      result.extents.push_back(std::move(e));
    }
    for (size_t i : ordered) {
      std::vector<Pointer> pointers;
      for (const Pointer& p : src.extents[i].pointers) {
        if (i == root && contains(locator.cut, p.field)) continue;
        const auto t = new_index.find(p.target);
        if (t != new_index.end()) pointers.push_back(Pointer{p.field, t->second});
      }
      result.extents[new_index[i]].pointers = std::move(pointers);
    }
    const size_t target = new_index[root];
    for (const auto& loc : r.locations) {
      if (loc.first >= result.extents.size())
        return fail(error, ErrorKind::NoSuchExtent, "extent index " + dec(loc.first) + " is out of range");
      result.extents[loc.first].pointers.push_back(Pointer{loc.second, target});
    }
    ResolvedLink resolved;
    resolved.symbol = r.name;
    resolved.locator = std::move(locator);
    resolved.status = found.status;
    resolved.source = found.file;
    resolved.extents.push_back(target);
    for (const auto& kv : new_index)
      if (kv.second != target) resolved.extents.push_back(kv.second);
    res.links.push_back(std::move(resolved));
    resolved_any = true;
  }

  if (resolved_any) result.invalidate_strings();
  res.sources = sources.read;
  res.archive = std::move(result);
  *out = std::move(res);
  return true;
}

bool difference(const Archive& a, const Archive& b, const Load& load, std::optional<std::string>* out, Error* error) {
  const auto na = sorted_ref_names(a), nb = sorted_ref_names(b);
  if (na != nb) { *out = links_differ(na, nb); return true; }
  Resolution ra, rb;
  if (!resolve(a, load, &ra, error) || !resolve(b, load, &rb, error)) return false;
  const size_t ca = ra.archive.reachable_from_roots().size(), cb = rb.archive.reachable_from_roots().size();
  if (ca != cb) { *out = "struct counts differ (" + dec(ca) + " vs " + dec(cb) + ")"; return true; }
  if (!same_structure(ra.archive, rb.archive)) { *out = std::string("content differs"); return true; }
  out->reset();
  return true;
}

std::vector<std::string> differences(const Archive& a, const Archive& b, size_t max) {
  std::vector<std::string> out;
  auto add = [&](std::string line) { if (out.size() < max) out.push_back(std::move(line)); };
  const std::vector<Digest> ha = extent_hashes(a), hb = extent_hashes(b);
  const std::vector<uint32_t> la = a.layout(), lb = b.layout();   // where each struct sits once serialized
  auto find_root = [](const Archive& x, const std::string& name) -> const Root* {
    for (const Root& r : x.roots) if (r.name == name) return &r;
    return nullptr;
  };
  for (const Root& r : a.roots) if (!find_root(b, r.name)) add("root " + r.name + " is only in the first");
  for (const Root& r : b.roots) if (!find_root(a, r.name)) add("root " + r.name + " is only in the second");
  const auto na = sorted_ref_names(a), nb = sorted_ref_names(b);
  if (na != nb) add(links_differ(na, nb));

  struct Step { size_t ea, eb; std::string path; };
  std::set<std::pair<size_t, size_t>> seen;
  std::deque<Step> queue;
  for (const Root& r : a.roots) {
    const Root* other = find_root(b, r.name);
    if (other && ha[r.extent] != hb[other->extent] && seen.insert({r.extent, other->extent}).second)
      queue.push_back({r.extent, other->extent, r.name});
  }
  while (!queue.empty() && out.size() < max) {
    const Step s = queue.front();
    queue.pop_front();
    const Extent& x = a.extents[s.ea];
    const Extent& y = b.extents[s.eb];
    const size_t lx = trimmed_len(x.data), ly = trimmed_len(y.data);
    const std::string where =
        " (file offsets 0x" + hex32((uint64_t)la[s.ea] + kHeaderSize) + " and 0x" + hex32((uint64_t)lb[s.eb] + kHeaderSize) + ")";
    if (lx != ly || !std::equal(x.data.begin(), x.data.begin() + (ptrdiff_t)lx, y.data.begin())) {
      size_t first = 0;
      while (first < lx && first < ly && x.data[first] == y.data[first]) ++first;
      add(s.path + ": bytes differ, " + dec(lx) + " vs " + dec(ly) + " bytes, first at +0x" + hex32(first) + where);
    }
    const std::vector<Pointer> px = sorted_by_field(x.pointers), py = sorted_by_field(y.pointers);
    std::vector<uint32_t> fx, fy;
    for (const Pointer& p : px) fx.push_back(p.field);
    for (const Pointer& p : py) fy.push_back(p.field);
    if (fx != fy) {
      std::string list;
      for (uint32_t f : fx) if (!contains(fy, f)) list += (list.empty() ? "" : ",") + ("-" + hex32(f));
      for (uint32_t f : fy) if (!contains(fx, f)) list += (list.empty() ? "" : ",") + ("+" + hex32(f));
      add(s.path + ": pointer fields differ (" + list + ")" + where);
    }
    for (const Pointer& p : px) {
      const auto q = std::find_if(py.begin(), py.end(), [&](const Pointer& o) { return o.field == p.field; });
      if (q == py.end() || ha[p.target] == hb[q->target]) continue;
      if (seen.insert({p.target, q->target}).second) queue.push_back({p.target, q->target, s.path + "/" + hex32(p.field)});
    }
  }
  if (out.empty() && archive_hash(a) != archive_hash(b)) add("content differs");
  return out;
}

// ---------------------------------------------------------------------------------------------
// Entry points

Status resolve_links(const std::string& file_name, std::vector<uint8_t>& contents, const ReadDisc& read_disc,
                     std::string* error, const std::function<void(const std::string&)>& log) {
  if (contents.size() < kHeaderSize) return Status::NoLinks;
  // Cheap exit for archives without an external reference table.
  if (be32(contents.data() + 0x10) == 0) return Status::NoLinks;

  const auto started = std::chrono::steady_clock::now();
  Archive archive;
  Error e;
  if (!Archive::parse(contents.data(), contents.size(), &archive, &e)) {
    if (log) log(file_name + " is not a parseable archive, serving it as is: " + e.message);
    return Status::NoLinks;
  }
  size_t links = 0;
  for (const ExternRef& r : archive.refs) links += Locator::is_link(r.name);
  if (!links) return Status::NoLinks;

  std::vector<std::string> disc_files;
  const Load load = [&](const std::string& name, std::vector<uint8_t>* out, std::string* reason) {
    if (!read_disc) { *reason = "no disc reader was provided"; return false; }
    out->clear();
    if (!read_disc(name, out) || out->empty()) { *reason = "not found on the disc"; return false; }
    disc_files.push_back(name);
    return true;
  };
  Resolution resolution;
  if (!resolve(archive, load, &resolution, &e)) {
    if (error) *error = e.message;
    return Status::Failed;
  }
  if (log)
    for (const std::string& warning : resolution.warnings) log(file_name + ": " + warning);
  std::vector<uint8_t> out;
  if (!resolution.archive.serialize(&out, &e)) {
    if (error) *error = "could not serialize the resolved file: " + e.message;
    return Status::Failed;
  }
  if (log) {
    size_t counts[5] = {};
    for (const ResolvedLink& l : resolution.links) ++counts[(int)l.status];
    std::string statuses;
    for (int s = 0; s < 5; ++s)
      if (counts[s]) statuses += (statuses.empty() ? "" : ", ") + dec(counts[s]) + " " + link_status_name((LinkStatus)s);
    std::string files;
    for (const std::string& f : disc_files) files += (files.empty() ? "" : ", ") + f;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    char took[32];
    std::snprintf(took, sizeof took, "%.1f ms", ms);
    log("resolved " + dec(links) + " link(s) in " + file_name + " (" + statuses + ") from " + files + " in " + took + ": " +
        dec(contents.size()) + " bytes in, " + dec(out.size()) + " bytes out");
  }
  contents = std::move(out);
  return Status::Resolved;
}

bool same(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, std::string* first_difference) {
  auto say = [&](const std::string& what) { if (first_difference) *first_difference = what; return false; };
  Archive x, y;
  Error e;
  if (!Archive::parse(a.data(), a.size(), &x, &e)) return say("the first is not an archive: " + e.message);
  if (!Archive::parse(b.data(), b.size(), &y, &e)) return say("the second is not an archive: " + e.message);
  const auto na = sorted_ref_names(x), nb = sorted_ref_names(y);
  if (na != nb) return say(links_differ(na, nb));
  const size_t ca = x.reachable_from_roots().size(), cb = y.reachable_from_roots().size();
  if (ca != cb) {
    const auto lines = differences(x, y, 1);
    return say("struct counts differ (" + dec(ca) + " vs " + dec(cb) + ")" + (lines.empty() ? "" : "; " + lines[0]));
  }
  if (!same_structure(x, y)) {
    const auto lines = differences(x, y, 1);
    return say(lines.empty() ? "content differs" : "content differs; " + lines[0]);
  }
  if (first_difference) first_difference->clear();
  return true;
}

}  // namespace slippi::hsd
