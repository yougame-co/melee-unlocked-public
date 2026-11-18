// Slippi game-file asset links (hle/slippi_hsd_links.cpp): the slippi-hsd crate's own tests on
// synthetic archives, case for case (archive.rs, hash.rs, link.rs), plus the entry points the host
// calls. With
//   --local <GameFiles dir> <extracted disc dir or disc image> [--ref <dir>] [--out <dir>]
// it also works on real files, as the crate's tests/local_files.rs: every disc archive must round
// trip byte for byte, both shipped files are resolved against the disc and reported link by status,
// and each result is compared structurally with the file of the same name in --ref (slpCSS.dat
// stands in for SlippiCSS_gui.dat, its name before Slippi 3.7.0).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "slippi_hsd_links.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

using namespace slippi::hsd;
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

namespace {

uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

// A tiny archive: root -> A (two pointers) -> B (buffer) and C (buffer, shared by A twice).
Archive sample() {
  Archive a;
  a.version = {'0', '0', '1', 'B'};
  Extent e0;
  e0.data.assign(16, 0);
  e0.pointers = {{0, 1}, {4, 2}, {8, 2}};
  Extent e1;
  for (int i = 1; i <= 40; ++i) e1.data.push_back((uint8_t)i);
  e1.phase = 16;
  e1.origin = 16;
  Extent e2;
  e2.data.assign(64, 7);
  e2.origin = 64;
  a.extents = {e0, e1, e2};
  a.roots = {Root{"sample_root", 0}};
  return a;
}

Bytes ser(const Archive& a) {
  Bytes out;
  Error e;
  if (!a.serialize(&out, &e)) { std::fprintf(stderr, "serialize: %s\n", e.message.c_str()); std::exit(2); }
  return out;
}
Archive parsed(const Bytes& b) {
  Archive a;
  Error e;
  if (!Archive::parse(b.data(), b.size(), &a, &e)) { std::fprintf(stderr, "parse: %s\n", e.message.c_str()); std::exit(2); }
  return a;
}
bool linked(Archive& a, size_t extent, const std::string& symbol) {
  Error e;
  if (!link(a, extent, symbol, &e)) { std::fprintf(stderr, "link: %s\n", e.message.c_str()); return false; }
  return true;
}
Load disc_loader(const Bytes& bytes) {
  return [bytes](const std::string& name, Bytes* out, std::string* reason) {
    if (name == "Sample.dat") { *out = bytes; return true; }
    *reason = "no such file";
    return false;
  };
}
Load files_loader(std::vector<std::pair<std::string, Bytes>> files) {
  return [files](const std::string& name, Bytes* out, std::string* reason) {
    for (const auto& f : files)
      if (f.first == name) { *out = f.second; return true; }
    *reason = "no such file";
    return false;
  };
}
// resolve() that must succeed.
bool resolved(const Archive& a, const Load& load, Resolution* r) {
  Error e;
  if (!resolve(a, load, r, &e)) { std::fprintf(stderr, "resolve: %s\n", e.message.c_str()); return false; }
  return true;
}
// resolve() that must fail; the error.
Error resolve_error(const Archive& a, const Load& load) {
  Resolution r;
  Error e;
  if (resolve(a, load, &r, &e)) e.kind = ErrorKind::None;
  return e;
}
bool contains_run(const Bytes& hay, const Bytes& needle) {
  return std::search(hay.begin(), hay.end(), needle.begin(), needle.end()) != hay.end();
}
bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }
void swap_targets_1_2(Extent& e) {
  for (Pointer& p : e.pointers) p.target = p.target == 1 ? 2 : p.target == 2 ? 1 : p.target;
}
std::vector<Pointer>& drop_target(std::vector<Pointer>& v, size_t target) {
  v.erase(std::remove_if(v.begin(), v.end(), [&](const Pointer& p) { return p.target == target; }), v.end());
  return v;
}

// ---- archive.rs ----
int serialize_then_parse_is_identity() {
  const Archive a = sample();
  const Bytes bytes = ser(a);
  const Archive b = parsed(bytes);
  CHECK(ser(b) == bytes);
  CHECK(b.extents.size() == 3);
  CHECK(b.extents[0].pointers.size() == 3);
  CHECK(b.roots[0].name == "sample_root");
  const auto offsets = b.layout();   // alignment phases survive
  CHECK(offsets[1] % kAlign == 16);
  CHECK(offsets[2] % kAlign == 0);
  return 0;
}

int extern_chain_round_trips() {
  Archive a = sample();
  // Turn both pointers to C into an external reference with a two-location chain.
  drop_target(a.extents[0].pointers, 2);
  a.refs.push_back(ExternRef{"ext_sym", {{0, 4}, {0, 8}}});
  Error e;
  CHECK(a.remove_extents({2}, &e));
  const Bytes bytes = ser(a);
  const Archive b = parsed(bytes);
  CHECK(b.refs.size() == 1);
  CHECK((b.refs[0].locations == std::vector<std::pair<size_t, uint32_t>>{{0, 4}, {0, 8}}));
  CHECK(ser(b) == bytes);
  CHECK(be32(bytes.data() + 8) == 1);   // chain fields are not relocation entries
  return 0;
}

// ---- hash.rs ----
int relayout_does_not_change_hash() {
  const Archive a = sample();
  Archive b = a;
  // Reorder extents 1 and 2, fix the pointers, pad extent 1 with zeros.
  std::swap(b.extents[1], b.extents[2]);
  swap_targets_1_2(b.extents[0]);
  b.extents[2].data.insert(b.extents[2].data.end(), 24, 0);
  CHECK(same_structure(a, b));
  CHECK(ser(a) != ser(b));
  return 0;
}

int content_change_changes_hash() {
  const Archive a = sample();
  Archive b = a;
  b.extents[2].data[3] = 9;
  CHECK(!same_structure(a, b));
  Archive c = a;
  Extent extra;
  extra.data = {1, 2, 3, 4};
  c.extents.push_back(extra);
  c.extents[0].pointers.push_back(Pointer{12, 3});
  CHECK(!same_structure(a, c));
  return 0;
}

// ---- link.rs ----
int locator_round_trip() {
  Locator l;
  Error e;
  CHECK(Locator::parse("slp:GmTou1p.dat:0x1a2b0:cut=c,4:sha=deadbeef", &l, &e));
  CHECK(l.file == "GmTou1p.dat");
  CHECK(l.anchor.kind == Anchor::Kind::Offset && l.anchor.offset == 0x1a2b0);
  CHECK((l.cut == std::vector<uint32_t>{0xc, 0x4}));
  CHECK(l.sha && *l.sha == "deadbeef");
  CHECK(l.symbol() == "slp:GmTou1p.dat:0x1a2b0:cut=c,4:sha=deadbeef");

  Locator p;
  CHECK(Locator::parse("slp:MnSlChr.usd:MnSelectChrDataTable/68/0/4*3/8:at=0x1234:sha=0badf00d:strict=1", &p, &e));
  Anchor want;
  want.kind = Anchor::Kind::Path;
  want.root = "MnSelectChrDataTable";
  want.hops = {0x68, 0, 4, 4, 4, 8};
  CHECK(p.anchor == want);
  CHECK(p.at && *p.at == 0x1234);
  CHECK(p.strict);
  CHECK(p.symbol() == "slp:MnSlChr.usd:MnSelectChrDataTable/68/0/4*3/8:at=0x1234:sha=0badf00d:strict=1");
  Locator t;
  CHECK(Locator::parse("slp:GmTitle.usd:ScTitle_fog_foganim/", &t, &e));
  CHECK(t.anchor.kind == Anchor::Kind::Path && t.anchor.root == "ScTitle_fog_foganim" && t.anchor.hops.empty());

  Locator bad;
  CHECK(!Locator::parse("slp:GmTou1p.dat", &bad, &e) && e.kind == ErrorKind::BadLocator);
  CHECK(!Locator::parse("slp:GmTou1p.dat:0x10", &bad, &e));
  CHECK(!Locator::parse("slp:GmTou1p.dat:0x40:bogus=1", &bad, &e));
  CHECK(!Locator::parse("slp:GmTou1p.dat:/4", &bad, &e));
  CHECK(!Locator::is_link("ScTitle_fog_foganim"));
  // Rust's number rules: an optional '+', an uppercase prefix, an upper case sha kept lower case.
  CHECK(Locator::parse("slp:A.dat:R/+c*+2:at=0X40:sha=DEADBEEF", &l, &e) && l.anchor.hops == std::vector<uint32_t>({0xc, 0xc}));
  CHECK(*l.at == 0x40 && *l.sha == "deadbeef");
  CHECK(!Locator::parse("slp:A.dat:R/c*:sha=deadbeef", &bad, &e) && has(e.message, "bad repeat count"));
  CHECK(!Locator::parse("slp:A.dat:R/c:sha=deadbee", &bad, &e) && has(e.message, "unknown option sha=deadbee"));
  CHECK(!Locator::parse("slp:A.dat:0x100000000", &bad, &e) && has(e.message, "bad number 0x100000000"));
  CHECK(!Locator::parse("slp::0x40", &bad, &e) && has(e.message, "missing file name"));
  return 0;
}

int locate_prefers_a_path() {
  const Archive a = sample();
  const Locator l = locate(a, "Sample.dat", 2, {});
  CHECK(l.anchor.kind == Anchor::Kind::Path && l.anchor.root == "sample_root" && l.anchor.hops == std::vector<uint32_t>{4});
  CHECK(l.at && *l.at == kHeaderSize + 64);
  CHECK(l.sha && *l.sha == asset_sha(a, 2, {}));
  size_t got = 0;
  std::string why;
  CHECK(follow(a, "sample_root", {4}, &got, &why) && got == 2);
  CHECK(follow(a, "sample_root", {8}, &got, &why) && got == 2);
  CHECK(!follow(a, "sample_root", {0xc}, &got, &why));
  return 0;
}

int difference_ignores_layout_but_not_content_or_links() {
  const Archive original = sample();
  const Bytes disc = ser(original);
  const std::string symbol = locate(original, "Sample.dat", 2, {}).symbol();
  Archive linked_a = original;
  CHECK(linked(linked_a, 2, symbol));
  std::optional<std::string> diff;
  Error e;

  // the same file with extra padding on its first struct
  Archive padded = linked_a;
  padded.extents[0].data.insert(padded.extents[0].data.end(), 32, 0);
  CHECK(difference(linked_a, padded, disc_loader(disc), &diff, &e) && !diff);

  Archive edited = linked_a;
  edited.extents[0].data[0] ^= 0xFF;
  CHECK(difference(linked_a, edited, disc_loader(disc), &diff, &e) && diff);

  CHECK(difference(linked_a, original, disc_loader(disc), &diff, &e) && diff && has(*diff, "links differ"));
  return 0;
}

int link_then_resolve_restores_structure() {
  const Archive original = sample();
  const Bytes disc = ser(original);
  Archive l = original;
  const std::string symbol = locate(original, "Sample.dat", 2, {}).symbol();
  CHECK(linked(l, 2, symbol));
  CHECK(l.extents.size() == 2);                 // the linked buffer is gone
  CHECK(l.refs.size() == 1);
  CHECK(l.refs[0].locations.size() == 2);       // both pointers to it became chain locations
  const Bytes bytes = ser(l);
  CHECK(!contains_run(bytes, Bytes(64, 7)));    // linked bytes are not in the file

  const Archive reparsed = parsed(bytes);
  Resolution r;
  CHECK(resolved(reparsed, disc_loader(disc), &r));
  CHECK(r.warnings.empty());
  CHECK(r.sources == std::vector<std::string>{"Sample.dat"});
  CHECK(r.archive.refs.empty());
  CHECK(r.links.size() == 1);
  CHECK(r.links[0].status == LinkStatus::Exact);
  CHECK(r.links[0].symbol == symbol);
  CHECK(r.links[0].extents.size() == 1);
  CHECK(r.archive.extents[r.links[0].extents[0]].data == original.extents[2].data);
  CHECK(same_structure(original, r.archive));
  const Bytes out = ser(r.archive);
  CHECK(same_structure(original, parsed(out)));
  return 0;
}

// A re-saved disc file with a different layout and an edited buffer: the path still finds the
// asset, the edit is picked up, and the hash mismatch is a warning.
int modified_source_is_followed_by_path() {
  const Archive original = sample();
  Archive modded = original;
  std::swap(modded.extents[1], modded.extents[2]);
  swap_targets_1_2(modded.extents[0]);
  modded.extents[0].data.insert(modded.extents[0].data.end(), 32, 0);   // shifts every later offset
  modded.extents[1].data[3] = 9;   // the linked buffer, now at index 1, is edited
  const Bytes modded_disc = ser(modded);

  Archive l = original;
  const std::string symbol = locate(original, "Sample.dat", 2, {}).symbol();
  CHECK(linked(l, 2, symbol));
  Resolution r;
  CHECK(resolved(l, disc_loader(modded_disc), &r));
  CHECK(r.warnings.size() == 1);
  CHECK(has(r.warnings[0], "differs"));
  CHECK(r.links[0].status == LinkStatus::Modified);
  size_t asset = 0;
  std::string why;
  CHECK(follow(r.archive, "sample_root", {4}, &asset, &why));
  CHECK(r.archive.extents[asset].data[3] == 9);   // the user's edit is what got resolved
  CHECK(!same_structure(original, r.archive));

  // strict links refuse instead
  Archive strict = original;
  Locator sl = locate(original, "Sample.dat", 2, {});
  sl.strict = true;
  CHECK(linked(strict, 2, sl.symbol()));
  CHECK(resolve_error(strict, disc_loader(modded_disc)).kind == ErrorKind::HashMismatch);

  // an offset anchor alone lands on the wrong struct after the relayout
  Archive by_offset = original;
  CHECK(linked(by_offset, 2, "slp:Sample.dat:0x60:strict=1:sha=00000000"));
  CHECK(resolve_error(by_offset, disc_loader(modded_disc)).kind != ErrorKind::None);
  return 0;
}

int fallback_offset_needs_a_matching_hash() {
  const Archive original = sample();
  const Bytes disc = ser(original);
  const std::string sha = asset_sha(original, 2, {});

  Archive ok = original;
  CHECK(linked(ok, 2, "slp:Sample.dat:no_such_root/4:at=0x60:sha=" + sha));
  Resolution r;
  CHECK(resolved(ok, disc_loader(disc), &r));
  CHECK(r.warnings.size() == 1);
  CHECK(has(r.warnings[0], "found the recorded asset at 0x60"));
  CHECK(r.links[0].status == LinkStatus::Fallback);
  CHECK(same_structure(original, r.archive));

  Archive bad = original;
  CHECK(linked(bad, 2, "slp:Sample.dat:no_such_root/4:at=0x60:sha=00000000"));
  CHECK(resolve_error(bad, disc_loader(disc)).kind == ErrorKind::BadPath);

  Archive none = original;
  CHECK(linked(none, 2, "slp:Sample.dat:no_such_root/4"));
  CHECK(resolve_error(none, disc_loader(disc)).kind == ErrorKind::BadPath);
  return 0;
}

// A mod that rebuilds the structure around an asset but keeps the asset: the path and the fallback
// offset both miss, and the asset is found by its hash where it now sits.
int moved_asset_is_found_by_hash() {
  const Archive original = sample();
  Archive l = original;
  CHECK(linked(l, 2, locate(original, "Sample.dat", 2, {}).symbol()));

  Archive moved = original;
  drop_target(moved.extents[0].pointers, 2);
  moved.extents[0].pointers.push_back(Pointer{0xc, 3});
  moved.extents[1].data.insert(moved.extents[1].data.end(), 32, 0);   // pushes the buffer off its recorded offset
  Extent holder;
  holder.data = {0, 0, 0, 0, 1, 1, 1, 1};
  holder.pointers = {Pointer{0, 2}};
  moved.extents.push_back(holder);
  const Bytes disc = ser(moved);

  Resolution r;
  CHECK(resolved(l, disc_loader(disc), &r));
  CHECK(r.links[0].status == LinkStatus::Fallback);
  CHECK(r.links[0].source == "Sample.dat");
  CHECK(r.warnings.size() == 1);
  CHECK(same_structure(original, r.archive));
  return 0;
}

// When the named file no longer holds the asset at all, the other region's copy of the file stands
// in: the recorded asset if it is there, else its asset at the same path.
int regional_copy_stands_in() {
  const Archive original = sample();
  Archive l = original;
  CHECK(linked(l, 2, locate(original, "Sample.usd", 2, {}).symbol()));

  // The American file was rebuilt without the buffer; the Japanese one is untouched.
  Archive gutted = original;
  drop_target(gutted.extents[0].pointers, 2);
  const Bytes usd = ser(gutted);
  const Bytes dat = ser(original);

  Resolution r;
  CHECK(resolved(l, files_loader({{"Sample.usd", usd}, {"Sample.dat", dat}}), &r));
  CHECK(r.links[0].status == LinkStatus::Fallback);
  CHECK(r.links[0].source == "Sample.dat");
  CHECK((r.sources == std::vector<std::string>{"Sample.usd", "Sample.dat"}));
  CHECK(same_structure(original, r.archive));

  // also when the named file is missing altogether
  CHECK(resolved(l, files_loader({{"Sample.dat", dat}}), &r));
  CHECK(r.links[0].source == "Sample.dat");

  // A Japanese asset that differs is still the closest thing to the recorded one.
  Archive regional = original;
  regional.extents[2].data[0] = 8;
  const Bytes jp = ser(regional);
  CHECK(resolved(l, files_loader({{"Sample.usd", usd}, {"Sample.dat", jp}}), &r));
  CHECK(r.links[0].status == LinkStatus::Regional);
  CHECK(r.archive.extents[r.links[0].extents[0]].data[0] == 8);

  // but a strict link wants the recorded asset or nothing
  Archive strict = original;
  Locator sl = locate(original, "Sample.usd", 2, {});
  sl.strict = true;
  CHECK(linked(strict, 2, sl.symbol()));
  CHECK(resolve_error(strict, files_loader({{"Sample.usd", usd}, {"Sample.dat", jp}})).kind == ErrorKind::BadPath);

  const Error e = resolve_error(l, files_loader({{"Sample.usd", usd}}));
  CHECK(e.kind == ErrorKind::BadPath && has(e.message, "not in Sample.usd"));
  return 0;
}

int resolve_rejects_missing_file_and_bad_offset() {
  const Archive original = sample();
  const Bytes disc = ser(original);
  Archive l = original;
  CHECK(linked(l, 2, "slp:Sample.dat:0x60"));
  const Load gone = [](const std::string&, Bytes*, std::string* reason) { *reason = "gone"; return false; };
  CHECK(resolve_error(l, gone).kind == ErrorKind::Source);
  Archive bad = original;
  CHECK(linked(bad, 2, "slp:Sample.dat:0x61"));
  CHECK(resolve_error(bad, disc_loader(disc)).kind == ErrorKind::NotAStruct);
  return 0;
}

int cut_stops_at_siblings() {
  // A -> B, B.next -> C. Link B with its next pointer cut: C must not come along.
  Archive a = sample();
  a.extents[1].pointers.push_back(Pointer{4, 2});
  drop_target(a.extents[0].pointers, 2);   // A no longer points at C directly
  const Bytes disc = ser(a);
  Archive l = a;
  CHECK(linked(l, 1, "slp:Sample.dat:sample_root/0:cut=4"));
  CHECK(l.extents.size() == 1);   // B and C both left the file
  Resolution r;
  CHECK(resolved(l, disc_loader(disc), &r));
  CHECK(r.archive.extents.size() == 2);   // only B came back
  CHECK(r.archive.extents[1].pointers.empty());   // the cut pointer is null
  return 0;
}

// ---- the host's entry points ----
int sha1_known_answers() {
  // combine() over bytes with no trailing zero and no children is plain SHA-1.
  auto sha = [](const std::string& s) { return combine(Bytes(s.begin(), s.end()), {}); };
  const Digest abc = sha("abc");
  CHECK(hex(abc.data(), abc.size()) == "a9993e364706816aba3e25717850c26c9cd0d89d");
  const Digest two = sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq");
  CHECK(hex(two.data(), two.size()) == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
  const Digest million = combine(Bytes(1000000, 'a'), {});
  CHECK(hex(million.data(), million.size()) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
  return 0;
}

int resolve_links_entry_point() {
  const Archive original = sample();
  const Bytes disc = ser(original);
  const ReadDisc read = [&](const std::string& name, Bytes* out) {
    if (name != "Sample.dat") return false;
    *out = disc;
    return true;
  };
  std::vector<std::string> lines;
  const auto log = [&](const std::string& line) { lines.push_back(line); };
  std::string error;

  // Not an archive, no reference table, or only ordinary references: untouched.
  Bytes tiny = {1, 2, 3};
  CHECK(resolve_links("x.dat", tiny, read, &error, log) == Status::NoLinks && tiny == Bytes({1, 2, 3}));
  Bytes plain = disc;
  CHECK(resolve_links("x.dat", plain, read, &error, log) == Status::NoLinks && plain == disc);
  Archive ordinary = original;
  drop_target(ordinary.extents[0].pointers, 2);
  ordinary.refs.push_back(ExternRef{"ext_sym", {{0, 4}, {0, 8}}});
  Error e;
  CHECK(ordinary.remove_extents({2}, &e));
  Bytes kept = ser(ordinary);
  const Bytes kept_before = kept;
  CHECK(resolve_links("x.dat", kept, read, &error, log) == Status::NoLinks && kept == kept_before);
  Bytes garbage(0x40, 0);
  garbage[0x13] = 1;   // a reference count, but the sizes do not add up
  CHECK(resolve_links("x.dat", garbage, read, &error, log) == Status::NoLinks && lines.size() == 1 &&
        has(lines[0], "not a parseable archive"));
  lines.clear();

  // Resolved: the same bytes as resolve() then serialize(), structurally the original.
  Archive l = original;
  CHECK(linked(l, 2, locate(original, "Sample.dat", 2, {}).symbol()));
  const Bytes linked_bytes = ser(l);
  Bytes contents = linked_bytes;
  CHECK(resolve_links("Sample_gui.dat", contents, read, &error, log) == Status::Resolved);
  Resolution r;
  CHECK(resolved(parsed(linked_bytes), disc_loader(disc), &r));
  CHECK(contents == ser(r.archive));
  std::string why;
  CHECK(same(contents, disc, &why));
  CHECK(lines.size() == 1 && has(lines[0], "resolved 1 link(s) in Sample_gui.dat (1 exact) from Sample.dat"));
  lines.clear();

  // A modified disc file: one line for the link, one summary.
  Archive modded = original;
  modded.extents[2].data[5] = 1;
  const Bytes modded_disc = ser(modded);
  contents = linked_bytes;
  CHECK(resolve_links("Sample_gui.dat", contents,
                      [&](const std::string&, Bytes* out) { *out = modded_disc; return true; }, &error, log) ==
        Status::Resolved);
  CHECK(lines.size() == 2 && has(lines[0], "Sample_gui.dat: slp:Sample.dat:") && has(lines[0], "differs"));
  CHECK(!same(contents, disc, &why) && has(why, "sample_root/4: bytes differ"));
  lines.clear();

  // Failed: untouched, and why.
  contents = linked_bytes;
  CHECK(resolve_links("Sample_gui.dat", contents, [](const std::string&, Bytes*) { return false; }, &error, log) ==
        Status::Failed);
  CHECK(contents == linked_bytes);
  CHECK(error == "could not read disc file Sample.dat: not found on the disc");
  CHECK(resolve_links("Sample_gui.dat", contents, ReadDisc(), &error, log) == Status::Failed &&
        error == "could not read disc file Sample.dat: no disc reader was provided");
  return 0;
}

int same_entry_point() {
  const Archive a = sample();
  Archive b = a;
  std::swap(b.extents[1], b.extents[2]);
  swap_targets_1_2(b.extents[0]);
  b.extents[2].data.insert(b.extents[2].data.end(), 24, 0);
  std::string why;
  CHECK(same(ser(a), ser(b), &why) && why.empty());
  Archive c = a;
  c.extents[1].data[0] = 0x55;
  CHECK(!same(ser(a), ser(c), &why) && has(why, "sample_root/0: bytes differ"));
  Archive d = a;
  d.roots[0].name = "other_root";
  CHECK(!same(ser(a), ser(d), &why));
  CHECK(!same(ser(a), Bytes{1, 2}, &why) && has(why, "the second is not an archive"));
  return 0;
}

// ---- --local ----
bool read_file(const fs::path& path, Bytes* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}
// A disc: an extracted folder, or a GameCube image read through its file table (root files).
struct Disc {
  fs::path dir, image;
  std::vector<std::pair<std::string, std::pair<uint32_t, uint32_t>>> files;   // name, (offset, size)

  bool open(const fs::path& path) {
    if (fs::is_directory(path)) { dir = path; return true; }
    image = path;
    std::ifstream in(path, std::ios::binary);
    uint8_t header[0x440];
    if (!in.read((char*)header, sizeof header)) return false;
    const uint32_t fst_offset = be32(header + 0x424), fst_size = be32(header + 0x428);
    Bytes fst(fst_size);
    if (!in.seekg(fst_offset) || !in.read((char*)fst.data(), fst_size) || fst_size < 12) return false;
    const uint32_t entries = be32(fst.data() + 8);
    const size_t strings = (size_t)entries * 12;
    if (strings > fst.size()) return false;
    for (uint32_t i = 1; i < entries;) {
      const uint8_t* e = fst.data() + (size_t)i * 12;
      if (e[0]) { i = std::max(i + 1, be32(e + 8)); continue; }   // a directory: skip what is in it
      const size_t name = strings + (be32(e) & 0xFFFFFF);
      if (name < fst.size()) files.push_back({std::string((const char*)fst.data() + name), {be32(e + 4), be32(e + 8)}});
      ++i;
    }
    return !files.empty();
  }
  std::vector<std::string> names() const {
    std::vector<std::string> out;
    if (!dir.empty()) {
      for (const auto& entry : fs::directory_iterator(dir)) out.push_back(entry.path().filename().u8string());
    } else {
      for (const auto& f : files) out.push_back(f.first);
    }
    return out;
  }
  bool read(const std::string& name, Bytes* out) const {
    if (!dir.empty()) return read_file(dir / fs::u8path(name), out);
    for (const auto& f : files) {
      if (f.first != name) continue;
      std::ifstream in(image, std::ios::binary);
      out->resize(f.second.second);
      return in.seekg(f.second.first) && (out->empty() || in.read((char*)out->data(), (std::streamsize)out->size()));
    }
    return false;
  }
};
Load dir_loader(const Disc& disc) {
  return [&disc](const std::string& name, Bytes* out, std::string* reason) {
    if (disc.read(name, out)) return true;
    *reason = "cannot read " + name;
    return false;
  };
}
double ms_since(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

// every_disc_archive_round_trips_byte_for_byte
int round_trip_disc(const Disc& disc) {
  size_t checked = 0, containers = 0;
  std::vector<std::string> failures;
  for (const std::string& name : disc.names()) {
    std::string ext = name.size() > 4 ? name.substr(name.size() - 4) : std::string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (ext != ".dat" && ext != ".usd") continue;
    Bytes bytes;
    if (!disc.read(name, &bytes)) { failures.push_back(name + ": unreadable"); continue; }
    Archive a;
    Error e;
    if (!Archive::parse(bytes.data(), bytes.size(), &a, &e)) {
      // Pl*AJ.dat files are containers of many concatenated archives, not one archive.
      if (e.kind == ErrorKind::SizeMismatch && bytes.size() >= 4 && be32(bytes.data()) < bytes.size()) { ++containers; continue; }
      failures.push_back(name + ": parse: " + e.message);
      continue;
    }
    Bytes out;
    if (!a.serialize(&out, &e)) failures.push_back(name + ": serialize: " + e.message);
    else if (out != bytes) failures.push_back(name + ": not byte-identical");
    else ++checked;
  }
  std::printf("disc round trip: %zu archives byte-identical, %zu containers skipped, %zu failures\n", checked, containers,
              failures.size());
  for (const auto& f : failures) std::printf("  %s\n", f.c_str());
  return failures.empty() && checked > 800 ? 0 : 1;
}

// The shipped copy resolved against the disc (the full form the crate's local tests start from).
bool full_copy(const fs::path& gamefiles, const Disc& disc, const std::string& name, Archive* out, bool* was_linked) {
  Bytes bytes;
  if (!read_file(gamefiles / name, &bytes)) return false;
  Archive a = parsed(bytes);
  *was_linked = !a.refs.empty();
  if (!*was_linked) { *out = a; return true; }
  Resolution r;
  if (!resolved(a, dir_loader(disc), &r)) return false;
  *out = r.archive;
  return true;
}

// Port of local_files.rs link_one_buffer_and_resolve.
int link_one_buffer_and_resolve(const fs::path& gamefiles, const Disc& disc) {
  Archive shipped;
  bool was_linked = false;
  CHECK(full_copy(gamefiles, disc, "GameSetup_gui.dat", &shipped, &was_linked));
  Bytes source_bytes;
  CHECK(disc.read("GmTou1p.usd", &source_bytes));
  const Archive source = parsed(source_bytes);
  auto distinctive = [](const uint8_t* w) {
    bool seen[256] = {};
    for (int i = 0; i < 64; ++i) seen[w[i]] = true;
    return std::count(std::begin(seen), std::end(seen), true) >= 12;
  };
  auto trimmed = [](const Bytes& d) { size_t n = d.size(); while (n && !d[n - 1]) --n; return n; };
  size_t target_extent = 0, source_extent = 0, n = 0;
  bool found = false;
  for (size_t i = 0; i < shipped.extents.size() && !found; ++i) {
    const Extent& e = shipped.extents[i];
    const size_t len = trimmed(e.data);
    if (!e.pointers.empty() || len < 1024 || !distinctive(e.data.data() + len / 2)) continue;
    for (size_t j = 0; j < source.extents.size(); ++j) {
      const Extent& s = source.extents[j];
      if (s.pointers.empty() && trimmed(s.data) == len && std::equal(s.data.begin(), s.data.begin() + (ptrdiff_t)len, e.data.begin())) {
        target_extent = i; source_extent = j; n = len; found = true;
        break;
      }
    }
  }
  CHECK(found);
  const Bytes payload(shipped.extents[target_extent].data.begin(), shipped.extents[target_extent].data.begin() + (ptrdiff_t)n);
  const Bytes sample_run(payload.begin() + (ptrdiff_t)(n / 2), payload.begin() + (ptrdiff_t)(n / 2 + 64));
  auto is_copy = [&](const Extent& e) { return e.data.size() >= n && std::equal(payload.begin(), payload.end(), e.data.begin()); };
  const Locator locator = locate(source, "GmTou1p.usd", source_extent, {});
  CHECK(locator.anchor.kind == Anchor::Kind::Path);   // a texture buffer is reachable from the root
  Archive l = shipped;
  for (;;) {
    const auto it = std::find_if(l.extents.begin(), l.extents.end(), is_copy);
    if (it == l.extents.end()) break;
    CHECK(linked(l, (size_t)(it - l.extents.begin()), locator.symbol()));
  }
  const Bytes linked_bytes = ser(l);
  CHECK(!contains_run(linked_bytes, sample_run));
  CHECK(linked_bytes.size() < ser(shipped).size());
  const Archive reparsed = parsed(linked_bytes);
  CHECK(reparsed.refs.size() == 1);
  Resolution r;
  CHECK(resolved(reparsed, dir_loader(disc), &r));
  CHECK(r.warnings.empty());
  CHECK(r.archive.refs.empty());
  CHECK(same_structure(shipped, r.archive));
  const Bytes out = ser(r.archive);
  CHECK(same_structure(shipped, parsed(out)));
  CHECK(contains_run(out, sample_run));

  // A modified disc file: relaid out, the linked texture edited. The path finds it, the edit comes
  // through, and only a warning is logged.
  Archive modded = source;
  modded.extents[0].data.insert(modded.extents[0].data.end(), 32, 0);
  Bytes edited(modded.extents[source_extent].data.begin(), modded.extents[source_extent].data.begin() + (ptrdiff_t)n);
  edited[n / 2] ^= 0xFF;
  std::copy(edited.begin(), edited.end(), modded.extents[source_extent].data.begin());
  const Bytes modded_bytes = ser(modded);
  CHECK(resolved(reparsed, [&](const std::string&, Bytes* o, std::string*) { *o = modded_bytes; return true; }, &r));
  CHECK(r.warnings.size() == 1);
  const Bytes out2 = ser(r.archive);
  CHECK(!contains_run(out2, sample_run));
  CHECK(contains_run(out2, Bytes(edited.begin() + (ptrdiff_t)(n / 2), edited.begin() + (ptrdiff_t)(n / 2 + 64))));
  return 0;
}

// Port of local_files.rs link_background_joint_set.
int link_background_joint_set(const fs::path& gamefiles, const Disc& disc) {
  Archive shipped;
  bool was_linked = false;
  CHECK(full_copy(gamefiles, disc, "GameSetup_gui.dat", &shipped, &was_linked));
  Bytes source_bytes;
  CHECK(disc.read("GmTou1p.dat", &source_bytes));
  const Archive source = parsed(source_bytes);
  // ScGamTour_scene_data -> JOBJDescs array (field 0) -> desc N (field 4*N).
  auto desc = [](const Archive& a, uint32_t n) -> size_t {
    const size_t scene = a.roots[0].extent;
    size_t array = SIZE_MAX, d = SIZE_MAX;
    for (const Pointer& p : a.extents[scene].pointers) if (p.field == 0) { array = p.target; break; }
    if (array == SIZE_MAX) return SIZE_MAX;
    for (const Pointer& p : a.extents[array].pointers) if (p.field == 4 * n) { d = p.target; break; }
    return d;
  };
  const size_t mine = desc(shipped, 0), theirs = desc(source, 5);
  CHECK(mine != SIZE_MAX && theirs != SIZE_MAX);
  const Locator locator = locate(source, "GmTou1p.dat", theirs, {});
  CHECK(locator.anchor.kind == Anchor::Kind::Path && locator.anchor.root == "ScGamTour_scene_data" &&
        locator.anchor.hops == std::vector<uint32_t>({0, 0x14}));
  Archive l = shipped;
  const size_t before = l.extents.size();
  CHECK(linked(l, mine, locator.symbol()));
  CHECK(before - l.extents.size() > 100);
  Resolution r;
  CHECK(resolved(l, dir_loader(disc), &r));
  const size_t resolved_desc = desc(r.archive, 0);
  CHECK(resolved_desc != SIZE_MAX);
  CHECK(extent_hash(r.archive, resolved_desc) == extent_hash(source, theirs));
  CHECK(same_structure(shipped, r.archive) == was_linked);
  CHECK(parsed(ser(r.archive)).extents.size() > 0);
  return 0;
}

int local(const fs::path& gamefiles, const Disc& disc, const fs::path& ref, const fs::path& out_dir) {
  int failures = round_trip_disc(disc);
  for (const char* name : {"GameSetup_gui.dat", "SlippiCSS_gui.dat"}) {
    Bytes input;
    if (!read_file(gamefiles / name, &input)) { std::printf("%s: cannot read\n", name); ++failures; continue; }
    const Archive archive = parsed(input);
    size_t links = 0;
    for (const ExternRef& r : archive.refs) links += Locator::is_link(r.name);
    const auto t0 = std::chrono::steady_clock::now();
    Resolution r;
    if (!resolved(archive, dir_loader(disc), &r)) { ++failures; continue; }
    const Bytes out = ser(r.archive);
    const double took = ms_since(t0);
    size_t counts[5] = {};
    for (const ResolvedLink& l : r.links) ++counts[(int)l.status];
    std::string sources;
    for (const auto& s : r.sources) sources += (sources.empty() ? "" : ", ") + s;
    std::printf("%s: %zu links (%zu exact, %zu modified, %zu fallback, %zu regional, %zu unchecked), %zu bytes in, %zu bytes out, "
                "%.1f ms; from %s\n",
                name, links, counts[0], counts[1], counts[2], counts[3], counts[4], input.size(), out.size(), took,
                sources.c_str());
    for (const auto& w : r.warnings) std::printf("  warning: %s\n", w.c_str());

    // The host's entry point gives the same bytes; the result is a clean, stable archive.
    Bytes via_host = input;
    std::string error;
    const auto t1 = std::chrono::steady_clock::now();
    const Status status = resolve_links(name, via_host,
        [&](const std::string& n, Bytes* o) { return disc.read(n, o); }, &error);
    const double host_ms = ms_since(t1);
    const Archive back = parsed(out);
    bool clean = true;
    for (const ExternRef& x : back.refs) clean &= !Locator::is_link(x.name);
    const bool stable = ser(back) == out;
    std::printf("  resolve_links: %s in %.1f ms, %s; output re-parses %s, %s\n",
                status == Status::Resolved ? "Resolved" : status == Status::NoLinks ? "NoLinks" : ("Failed: " + error).c_str(),
                host_ms, via_host == out ? "same bytes" : "DIFFERENT bytes", clean ? "with no links left" : "WITH LINKS LEFT",
                stable ? "round trips" : "DOES NOT ROUND TRIP");
    if (status != Status::Resolved || via_host != out || !clean || !stable || r.archive.refs.size() != archive.refs.size() - links)
      ++failures;
    if (!out_dir.empty()) {
      fs::create_directories(out_dir);
      std::ofstream(out_dir / name, std::ios::binary).write((const char*)out.data(), (std::streamsize)out.size());
    }

    if (!ref.empty()) {
      fs::path ref_path = ref / name;
      if (!fs::exists(ref_path) && std::string(name) == "SlippiCSS_gui.dat") ref_path = ref / "slpCSS.dat";
      Bytes reference;
      if (!read_file(ref_path, &reference)) { std::printf("  reference %s: cannot read\n", ref_path.u8string().c_str()); continue; }
      std::string why;
      const bool is_same = same(out, reference, &why);
      const Archive ra = parsed(reference);
      std::printf("  vs %s (%zu bytes, %zu structs from its roots; ours %zu): %s\n", ref_path.filename().u8string().c_str(),
                  reference.size(), ra.reachable_from_roots().size(), r.archive.reachable_from_roots().size(),
                  is_same ? "SAME structurally" : ("DIFFERENT: " + why).c_str());
      if (!is_same) {
        const auto all = differences(r.archive, ra, 100000);
        std::printf("  %zu differing structs; the first %zu:\n", all.size(), std::min<size_t>(all.size(), 40));
        for (size_t i = 0; i < all.size() && i < 40; ++i) std::printf("    %s\n", all[i].c_str());
      }
    }
  }
  const auto t = [](int f) { return f ? "FAIL" : "PASS"; };
  const int one = link_one_buffer_and_resolve(gamefiles, disc);
  std::printf("link_one_buffer_and_resolve: %s\n", t(one));
  const int bg = link_background_joint_set(gamefiles, disc);
  std::printf("link_background_joint_set: %s\n", t(bg));
  return failures + one + bg;
}

}  // namespace

int main(int argc, char** argv) {
  struct Test { const char* name; int (*run)(); };
  const Test tests[] = {
      {"serialize_then_parse_is_identity", serialize_then_parse_is_identity},
      {"extern_chain_round_trips", extern_chain_round_trips},
      {"relayout_does_not_change_hash", relayout_does_not_change_hash},
      {"content_change_changes_hash", content_change_changes_hash},
      {"locator_round_trip", locator_round_trip},
      {"locate_prefers_a_path", locate_prefers_a_path},
      {"difference_ignores_layout_but_not_content_or_links", difference_ignores_layout_but_not_content_or_links},
      {"link_then_resolve_restores_structure", link_then_resolve_restores_structure},
      {"modified_source_is_followed_by_path", modified_source_is_followed_by_path},
      {"fallback_offset_needs_a_matching_hash", fallback_offset_needs_a_matching_hash},
      {"moved_asset_is_found_by_hash", moved_asset_is_found_by_hash},
      {"regional_copy_stands_in", regional_copy_stands_in},
      {"resolve_rejects_missing_file_and_bad_offset", resolve_rejects_missing_file_and_bad_offset},
      {"cut_stops_at_siblings", cut_stops_at_siblings},
      {"sha1_known_answers", sha1_known_answers},
      {"resolve_links_entry_point", resolve_links_entry_point},
      {"same_entry_point", same_entry_point},
  };
  int failed = 0;
  for (const Test& t : tests) {
    if (t.run()) { std::fprintf(stderr, "FAIL %s\n", t.name); ++failed; }
  }
  std::printf("%d of %zu tests passed\n", (int)(sizeof tests / sizeof *tests) - failed, sizeof tests / sizeof *tests);

  if (argc >= 4 && std::strcmp(argv[1], "--local") == 0) {
    fs::path ref, out;
    for (int i = 4; i + 1 < argc; i += 2) {
      if (std::strcmp(argv[i], "--ref") == 0) ref = fs::u8path(argv[i + 1]);
      else if (std::strcmp(argv[i], "--out") == 0) out = fs::u8path(argv[i + 1]);
    }
    Disc disc;
    if (!disc.open(fs::u8path(argv[3]))) {
      std::printf("cannot read the disc %s\n", argv[3]);
      return 1;
    }
    failed += local(fs::u8path(argv[2]), disc, ref, out);
  }
  return failed ? 1 : 0;
}
