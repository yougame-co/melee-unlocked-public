// SPDX-License-Identifier: GPL-2.0-or-later
// Slippi game-file asset links (Slippi 3.7.0). Slippi ships GameSetup_gui.dat and
// SlippiCSS_gui.dat with their Nintendo-authored assets replaced by external symbols
//
//   slp:<disc file>:<anchor>[:at=0x<file offset>][:cut=<field>[,<field>...]][:sha=<8 hex>][:strict=1]
//
// and copies those assets in from the player's own disc when the file is loaded. This is a port of
// the resolve path of Slippi's Rust crate slippi-hsd (archive.rs, link.rs, hash.rs): the same
// archive model, the same fallbacks and the same output bytes. Standalone: no host dependencies.
//
// The archive model: an archive (.dat / .usd) is a 0x20-byte header, a data section, a relocation
// table listing every pointer field in the data section, the exported root symbols, the imported
// external symbols and a string table. The data section is held as extents, the byte ranges between
// consecutive pointer targets, so assets move between archives by copying extents and rebasing
// pointers, with no knowledge of the structures inside.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace slippi::hsd {

enum class Status { NoLinks, Resolved, Failed };
// read_disc(name, out): the named file from the player's disc (e.g. "MnSlChr.usd"); false when missing.
using ReadDisc = std::function<bool(const std::string& name, std::vector<uint8_t>* out)>;
// Resolves every slp: link in `contents` in place. NoLinks leaves it untouched (also a file that is
// not an archive). Failed leaves it untouched and says why in *error. `log`, when given, gets one
// line per notable event (a modified, fallback or regional link, never one per exact link) and one
// summary line per resolved file.
Status resolve_links(const std::string& file_name, std::vector<uint8_t>& contents, const ReadDisc& read_disc,
                     std::string* error, const std::function<void(const std::string&)>& log = {});
// True when two archives hold the same assets structurally: the same external symbols and the same
// structures reachable from their roots, whatever the byte layout and padding (Slippi's `difference`
// for archives that are already resolved; links are compared by their symbols). Otherwise false
// and *first_difference says what differs.
bool same(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, std::string* first_difference);

// ---- The parts, for tests and tools ----

constexpr uint32_t kHeaderSize = 0x20;      // fixed header before the data section
constexpr uint32_t kChainEnd = 0xFFFFFFFFu; // ends an external reference chain
constexpr uint32_t kAlign = 32;             // archives load 32-byte aligned; textures need it
extern const char kLinkPrefix[];            // "slp:"

enum class ErrorKind {
  None, Truncated, SizeMismatch, PointerOutOfRange, RootOutOfRange, FieldStraddlesBoundary, BadChain,
  EmptyRef, BadLocator, Source, NotAStruct, BadPath, HashMismatch, CannotLinkRoot, Unreferenced,
  RefInsideLinkedSubtree, NoSuchExtent,
};
struct Error {
  ErrorKind kind = ErrorKind::None;
  std::string message;
};

// A pointer field inside an extent, targeting the start of another extent.
struct Pointer {
  uint32_t field = 0;  // offset of the 4-byte field within the extent
  size_t target = 0;   // index of the target extent
};
// A run of bytes between two pointer targets. Pointer and chain fields are zero in `data`.
struct Extent {
  std::vector<uint8_t> data;
  uint32_t phase = 0;   // data offset modulo 32 this extent must be placed at
  std::vector<Pointer> pointers;
  uint32_t origin = 0;  // data offset in the archive it was parsed from (informational)
  uint32_t len() const { return (uint32_t)data.size(); }
};
struct Root {
  std::string name;
  size_t extent = 0;
};
// An imported symbol and the (extent, field) locations that receive its address, in chain order.
struct ExternRef {
  std::string name;
  std::vector<std::pair<size_t, uint32_t>> locations;
};
struct StringTable {
  std::vector<uint8_t> bytes;
  std::vector<uint32_t> root_offsets;
  std::vector<uint32_t> ref_offsets;
};

struct Archive {
  std::array<uint8_t, 4> version{};
  std::array<uint8_t, 8> header_tail{};  // header bytes 0x18..0x20, kept verbatim
  std::vector<Extent> extents;
  std::vector<Root> roots;
  std::vector<ExternRef> refs;
  std::optional<StringTable> strings;    // the parsed string table, reused while the symbols are unchanged

  // Lossless: serializing an unmodified archive reproduces the input byte for byte.
  static bool parse(const uint8_t* bytes, size_t size, Archive* out, Error* error);
  bool serialize(std::vector<uint8_t>* out, Error* error) const;
  void invalidate_strings() { strings.reset(); }
  // Index of the extent that started at `data_offset` in the parsed file.
  std::optional<size_t> extent_at_origin(uint32_t data_offset) const;
  // Data offsets the extents would be serialized at.
  std::vector<uint32_t> layout() const;
  // Extents reachable from `starts`, breadth first. `cut` may veto a pointer (holding extent, pointer).
  std::vector<size_t> reachable(const std::vector<size_t>& starts,
                                const std::function<bool(size_t, const Pointer&)>& cut = {}) const;
  std::vector<size_t> reachable_from_roots() const;
  // Drops the given extents and renumbers the rest; fails while anything still needs one of them.
  bool remove_extents(const std::vector<size_t>& remove, Error* error);
};

// Structural hashes: a struct's trimmed bytes plus its pointer fields and their targets' hashes.
using Digest = std::array<uint8_t, 20>;
Digest combine(const std::vector<uint8_t>& data, const std::vector<std::pair<uint32_t, Digest>>& children);
Digest extent_hash(const Archive& archive, size_t extent);
// Every extent's hash with one shared memo (the order Slippi uses to pick hash-search candidates).
std::vector<Digest> extent_hashes(const Archive& archive);
Digest archive_hash(const Archive& archive);
bool same_structure(const Archive& a, const Archive& b);
std::string hex(const uint8_t* bytes, size_t size);

struct Anchor {
  enum class Kind { Offset, Path } kind = Kind::Offset;
  uint32_t offset = 0;          // Offset: absolute file offset of the struct
  std::string root;             // Path: a root symbol...
  std::vector<uint32_t> hops;   // ...and the pointer fields to follow from it
  std::string text() const;     // as written in a symbol: 0x<offset> or <root>/<hops>
  bool operator==(const Anchor& o) const {
    return kind == o.kind && (kind == Kind::Offset ? offset == o.offset : root == o.root && hops == o.hops);
  }
};
struct Locator {
  std::string file;
  Anchor anchor;
  std::optional<uint32_t> at;     // file offset tried when a path anchor does not resolve
  std::vector<uint32_t> cut;      // pointer fields of the first struct that are not followed
  std::optional<std::string> sha; // first 8 hex digits of the asset's structural hash
  bool strict = false;            // a hash mismatch is an error rather than a warning
  static bool is_link(const std::string& symbol);
  static bool parse(const std::string& symbol, Locator* out, Error* error);
  std::string symbol() const;
};

enum class LinkStatus { Exact, Modified, Fallback, Regional, Unchecked };
const char* link_status_name(LinkStatus status);
struct ResolvedLink {
  std::string symbol;
  Locator locator;
  LinkStatus status = LinkStatus::Exact;
  std::string source;           // disc file the asset came from (the locator's or its other region's)
  std::vector<size_t> extents;  // extents copied in for this link, the linked struct first
};
struct Resolution {
  Archive archive;
  std::vector<std::string> warnings;  // links that resolved to something other than the recorded asset
  std::vector<std::string> sources;   // disc files read, in order
  std::vector<ResolvedLink> links;    // every slp: link, in reference table order
};
// load(name, out, reason): a disc file's bytes, or false with the reason.
using Load = std::function<bool(const std::string& name, std::vector<uint8_t>* out, std::string* reason)>;

std::string asset_sha(const Archive& archive, size_t extent, const std::vector<uint32_t>& cut);
bool follow(const Archive& archive, const std::string& root, const std::vector<uint32_t>& hops, size_t* out,
            std::string* reason);
// Authoring side, used by the tests: a locator for an extent, and turning an extent into a link.
Locator locate(const Archive& archive, const std::string& file, size_t extent, const std::vector<uint32_t>& cut);
bool link(Archive& archive, size_t extent, const std::string& symbol, Error* error);
bool resolve(const Archive& archive, const Load& load, Resolution* out, Error* error);
// Slippi's `difference`: nullopt when the two linked archives match once resolved, else what differs.
bool difference(const Archive& a, const Archive& b, const Load& load, std::optional<std::string>* out, Error* error);
// Structs that differ between two archives, walking both from their roots in step: one line each,
// with the path from the root, at most `max` lines.
std::vector<std::string> differences(const Archive& a, const Archive& b, size_t max);

}  // namespace slippi::hsd
