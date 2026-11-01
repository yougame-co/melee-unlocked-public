// SPDX-License-Identifier: GPL-2.0-or-later
#include "handoff_state.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s (%s)\n", __LINE__, #x, why.c_str()); return 1; } } while (0)
using namespace handoff_state;
int main(int argc, char** argv) {
  std::string why;
  CHECK(argc == 2);
  const auto path = std::filesystem::path(argv[1]) / "handoff_state_test.bin";
  { std::ofstream out(path, std::ios::binary); out << "abc"; }
  Identity id;
  CHECK(library_identity(path, id, why));
  const uint8_t sha[32] = {0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
    0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
  CHECK(id.size == 3 && std::equal(id.sha256.begin(), id.sha256.end(), sha));
  CHECK(!library_identity(path / "absent", id, why) && why == "cannot open game library");
  { std::ofstream out(path, std::ios::binary | std::ios::trunc); }
  CHECK(!library_identity(path, id, why) && why == "invalid game library size");

  std::vector<uint8_t> heap(8192, 0x37);
  std::vector<native_savestate::Span> spans{{heap.data(), 3000}, {heap.data()+4096, 4096}};
  const auto hash = native_savestate::hash_spans(spans);
  State state{123456789012ull, 2, 0, {1, 2, 3, 4}};
  Sizes sizes;
  std::vector<uint8_t> file;
  CHECK(encode(id, spans, state, file, sizes, why));
  CHECK(sizes.raw == 7096 && sizes.stored < sizes.raw);
  CHECK(sizes.file == header_size + 2 * table_entry_size + state.host.size() + sizes.stored);
  std::printf("handoff_state synthetic fixture: raw %llu, compressed %llu, file %llu, ratio %.4f\n",
    static_cast<unsigned long long>(sizes.raw), static_cast<unsigned long long>(sizes.stored),
    static_cast<unsigned long long>(sizes.file), double(sizes.stored)/double(sizes.raw));
  const auto original = file;
  const auto base = reinterpret_cast<uintptr_t>(heap.data());
  Range parsed;
  CHECK(parse_range("0x80000000", 8, parsed) && parsed.address == 0x80000000 && parsed.size == 8);
  for (const char* bad_address : {"", "0x", "1234", "0x-1", "0x 1", "0x0", "0xG", "0x10000000000000000"})
    CHECK(!parse_range(bad_address, 1, parsed));
  CHECK(!parse_range("0x1", 0, parsed));
  CHECK(!parse_range("0x1", uint64_t(UINT32_MAX)+1, parsed));
  CHECK(!parse_range("0xffffffffffffffff", 1, parsed));
  CHECK(validate_keep({{base, 3000}, {base+4096, 4096}, {base+20, 4}}, spans, why));
  CHECK(!validate_keep({{base+2999, 2}}, spans, why) && why == "handoffKeep range outside snapshot spans");
  CHECK(!validate_keep({{base+8191, 2}}, spans, why));
  CHECK(!validate_keep({{base, 0}}, spans, why) && why == "invalid handoffKeep range");
  CHECK(!validate_keep({{UINTPTR_MAX, 1}}, spans, why));
  std::vector<native_savestate::Span> adjacent{{heap.data(), 20}, {heap.data()+20, 20}};
  CHECK(validate_keep({{base+19, 2}}, adjacent, why));
  State gate_state{state.frame, state.scene_major, state.scene_minor, {}};
  Gate gate{state.frame, state.scene_major, state.scene_minor};
  CHECK(validate_gate(gate_state, gate, why));
  gate.disc_pending = true;
  CHECK(!validate_gate(gate_state, gate, why) && why == "disc read/callback pending");
  gate.disc_pending = false; gate.callback_pending = true;
  CHECK(!validate_gate(gate_state, gate, why) && why == "guest callback pending");
  gate.callback_pending = false; ++gate.frame;
  CHECK(!validate_gate(gate_state, gate, why) && why == "different frame count");
  --gate.frame; ++gate.scene_minor;
  CHECK(!validate_gate(gate_state, gate, why) && why == "different scene");
  --gate.scene_minor; ++gate.scene_major;
  CHECK(!validate_gate(gate_state, gate, why) && why == "different scene");
  --gate.scene_major;
  CHECK(!validate_gate(state, gate, why) && why == "unsupported host state record");
  std::fill(heap.begin(), heap.end(), 0xa5);
  State loaded;
  CHECK(load(file, id, spans, loaded, sizes, why));
  CHECK(native_savestate::hash_spans(spans) == hash && heap[3500] == 0xa5);
  CHECK(loaded.frame == state.frame && loaded.scene_major == 2 && loaded.scene_minor == 0 && loaded.host == state.host);
  // Gate refusals happen before writes; B's kept bytes survive a successful cross-state load.
  std::vector<uint8_t> gated_file;
  CHECK(encode(id, spans, gate_state, gated_file, sizes, why));
  std::fill(heap.begin(), heap.end(), 0x5a);
  const auto before_gate = heap;
  State gated_loaded;
  ++gate.frame;
  CHECK(!load(gated_file, id, spans, gated_loaded, sizes, why, &gate) && heap == before_gate);
  CHECK(why == "different frame count");
  --gate.frame;
  CHECK(!load(gated_file, id, spans, gated_loaded, sizes, why, &gate, {{base+2999, 2}}) && heap == before_gate);
  CHECK(load(gated_file, id, spans, gated_loaded, sizes, why, &gate, {{base+8, 8}, {base+12, 8}}));
  CHECK(heap[0] == 0x37 && heap[7] == 0x37 && heap[8] == 0x5a && heap[19] == 0x5a && heap[20] == 0x37);
  auto refusal = [&](std::vector<uint8_t> bad, const char* reason, Identity identity) {
    const auto before = heap;
    State untouched{99, 98, 97, {96}};
    const bool ok = !load(bad, identity, spans, untouched, sizes, why) && why == reason &&
        before == heap && untouched.frame == 99 && untouched.scene_major == 98 &&
        untouched.scene_minor == 97 && untouched.host == std::vector<uint8_t>{96};
    return ok;
  };
  auto changed = [&](size_t offset, const char* reason) {
    auto bad = original; bad[offset] ^= 1; return refusal(std::move(bad), reason, id);
  };
  CHECK(changed(0, "wrong magic")); CHECK(changed(4, "unsupported version"));
  CHECK(changed(8, "different game library identity")); CHECK(changed(16, "different game library identity"));
  CHECK(changed(48, "different span count")); CHECK(changed(header_size, "different span table"));
  CHECK(changed(header_size+8, "different span table"));
  CHECK(changed(70, "host state exceeds size limit"));
  CHECK(changed(73, "unsupported codec")); CHECK(changed(76, "different raw span size"));
  auto bad = original; std::fill(bad.begin()+84, bad.begin()+92, 0);
  CHECK(refusal(bad, "invalid stored size", id));
  bad = original; bad[84] = 0xff; bad[85] = 0xff;
  CHECK(refusal(bad, "invalid stored size", id));
  CHECK(changed(92, "payload checksum mismatch"));
  CHECK(changed(header_size+2*table_entry_size, "payload checksum mismatch"));
  bad = original; bad[header_size+2*table_entry_size+state.host.size()] = 0;
  CHECK(refusal(bad, "invalid compressed payload", id));
  bad = original; bad.push_back(0);
  CHECK(refusal(bad, "oversized file", id));
  // Every truncation, including the span table, host record, and the last payload byte.
  for (size_t n = 0; n < original.size(); ++n) {
    bad.assign(original.begin(), original.begin()+n);
    CHECK(refusal(bad, n < header_size ? "truncated header" : "truncated file", id));
  }
  // Incompressible data exercises raw fallback and raw payload corruption/length validation.
  std::mt19937 rng(123);
  for (auto& byte : heap) byte = uint8_t(rng());
  const auto random_hash = native_savestate::hash_spans(spans);
  CHECK(encode(id, spans, state, file, sizes, why) && file[72] == 0 && sizes.stored == sizes.raw);
  bad = file; bad.back() ^= 1;
  CHECK(refusal(bad, "payload checksum mismatch", id));
  bad = file; --bad[84];
  CHECK(refusal(bad, "invalid stored size", id));
  std::fill(heap.begin(), heap.end(), 0);
  CHECK(load(file, id, spans, loaded, sizes, why) && native_savestate::hash_spans(spans) == random_hash);

  // Bounded I/O and dump bytes (raw, in table order).
  FILE* f = std::fopen(path.string().c_str(), "wb");
  CHECK(f && write_file(f, file, why)); CHECK(std::fclose(f) == 0);
  std::vector<uint8_t> read;
  f = std::fopen(path.string().c_str(), "rb");
  CHECK(f && read_file(f, spans, read, why) && read == file); CHECK(std::fclose(f) == 0);
  CHECK(!write_file(nullptr, file, why) && why == "snapshot write failed");
  CHECK(!read_file(nullptr, spans, read, why) && why == "cannot open snapshot");
  f = std::fopen(path.string().c_str(), "wb");
  CHECK(f && dump(f, spans, why)); CHECK(std::fclose(f) == 0);
  f = std::fopen(path.string().c_str(), "rb");
  CHECK(f && read_file(f, spans, read, why)); CHECK(std::fclose(f) == 0);
  CHECK(read.size() == 20 + 24 + 7096 && !std::memcmp(read.data(), "MUHD", 4));
  CHECK(!std::memcmp(read.data()+44, heap.data(), 3000));
  CHECK(!std::memcmp(read.data()+44+3000, heap.data()+4096, 4096));
  f = std::fopen(path.string().c_str(), "wb");
  CHECK(f);
  std::vector<uint8_t> huge(header_size+24+max_host_size+7097);
  CHECK(write_file(f, huge, why)); CHECK(std::fclose(f) == 0);
  f = std::fopen(path.string().c_str(), "rb");
  CHECK(f && !read_file(f, spans, read, why) && why == "oversized file"); CHECK(std::fclose(f) == 0);
  std::filesystem::remove(path);

  State oversized; oversized.host.resize(max_host_size+1);
  CHECK(!encode(id, spans, oversized, file, sizes, why) && why == "host state exceeds size limit");
  CHECK(!encode(id, {}, state, file, sizes, why) && why == "invalid span count");
  std::vector<native_savestate::Span> invalid{{nullptr, 1}};
  CHECK(!encode(id, invalid, state, file, sizes, why) && why == "invalid or unordered live span table");
  invalid = {{heap.data(), 0}};
  CHECK(!encode(id, invalid, state, file, sizes, why) && why == "invalid or unordered live span table");
  invalid = {{heap.data()+4, 8}, {heap.data(), 8}};
  CHECK(!encode(id, invalid, state, file, sizes, why) && why == "invalid or unordered live span table");
  invalid = {{reinterpret_cast<uint8_t*>(~uintptr_t(0)-1), 8}};
  CHECK(!encode(id, invalid, state, file, sizes, why) && why == "invalid or unordered live span table");
  invalid = {{heap.data(), uint32_t(max_raw_size+1)}};
  CHECK(!encode(id, invalid, state, file, sizes, why) && why == "snapshot exceeds size limit");
  std::printf("handoff_state: round trips, hashes, SHA-256, refusals, bounded I/O and raw dump passed\n");
  return 0;
}
