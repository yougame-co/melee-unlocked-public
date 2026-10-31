// Entirely synthetic images: no retail disc bytes and no game process.
#include "sysgen_disc.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

using Bytes = std::vector<uint8_t>;
namespace fs = std::filesystem;
static int checks = 0;
static void check(bool ok, const char* message) {
  ++checks;
  if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
static void be32(Bytes& image, size_t at, uint32_t value) {
  for (int i = 0; i < 4; ++i) image[at + i] = uint8_t(value >> (24 - i * 8));
}
static Bytes fixture() {
  Bytes image(0x1000);
  be32(image, 0x420, 0x900);
  be32(image, 0x424, 0x500);
  be32(image, 0x428, 68);
  be32(image, 0x500, 0x01000000);  // root, directory, two files
  be32(image, 0x508, 4);
  be32(image, 0x50c, 0x01000000);
  be32(image, 0x514, 3);           // nested first file
  be32(image, 0x518, 0);
  be32(image, 0x51c, 0x800);
  be32(image, 0x520, 3);
  be32(image, 0x524, 10);
  be32(image, 0x528, 0x810);
  be32(image, 0x52c, 4);
  const char names[] = "asset.dat\0ASSET.DAT";
  std::memcpy(image.data() + 0x530, names, sizeof names);
  std::memcpy(image.data() + 0x800, "one", 3);
  std::memcpy(image.data() + 0x810, "TWO!", 4);
  std::memcpy(image.data() + 0x900, "FAKEDOL!", 8);
  return image;
}
static void write(const fs::path& path, const Bytes& image) {
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char*>(image.data()), image.size());
  check(bool(file), "write synthetic image");
}
static bool fake_dol(const Bytes& image) {
  return image == Bytes({'F', 'A', 'K', 'E', 'D', 'O', 'L', '!'});
}
int main() {
  const auto root = fs::temp_directory_path() / ("mu-disc-reader-" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(root);
  struct Cleanup { fs::path path; ~Cleanup() { fs::remove_all(path); } } cleanup{root};
  const auto path = root / "synthetic.iso";
  const auto image = fixture();
  write(path, image);
  {
    host::DiscImageReader reader(path.u8string());
    uint32_t offset = 0, size = 0;
    Bytes out;
    check(reader.find_file("ASSET.DAT", &offset, &size) && offset == 0x810 && size == 4,
          "exact name wins over earlier case-insensitive match");
    check(reader.read_file("asset.dat", 3, out) && out == Bytes({'o', 'n', 'e'}), "nested exact file");
    check(reader.read_file("aSSet.dAT", 3, out) && out == Bytes({'o', 'n', 'e'}), "case-folded lookup");
    check(!reader.read_file("ASSET.DAT", 3, out), "size mismatch refused");
    check(!reader.find_file("missing", nullptr, nullptr), "missing name refused");
    check(reader.read_dol(8, out) && fake_dol(out), "DOL offset comes from image header");
    check(!reader.read_dol(SIZE_MAX, out), "oversized DOL refused before allocation");
  }
  auto refused = [&](const Bytes& data, const char* message) {
    write(path, data);
    host::DiscImageReader reader(path.u8string());
    Bytes out;
    check(!reader.read_file("asset.dat", 3, out), message);
  };
  refused(Bytes(0x430), "truncated header refused");
  refused(Bytes(image.begin(), image.begin() + 0x520), "truncated FST refused");
  refused(Bytes(image.begin(), image.begin() + 0x802), "truncated file refused");
  auto bad = image;
  be32(bad, 0x424, 0xfffffff0u);
  refused(bad, "FST offset past EOF refused");
  bad = image; be32(bad, 0x428, 0xffffffffu);
  refused(bad, "FST size past EOF refused before allocation");
  bad = image; be32(bad, 0x51c, 0xfffffff0u);
  refused(bad, "file offset past EOF refused");
  bad = image; be32(bad, 0x520, 0xffffffffu);
  refused(bad, "file extent cannot wrap");
  bad = image; be32(bad, 0x508, 0xffffffffu);
  refused(bad, "invalid entry count refused");
  bad = image; be32(bad, 0x518, 0xffffffu);
  refused(bad, "string offset outside FST refused");
  for (bool past_eof : {false, true}) {
    bad = image;
    if (past_eof) be32(bad, 0x420, 0xfffffff0u);
    else bad.resize(0x904);
    write(path, bad);
    host::DiscImageReader reader(path.u8string());
    Bytes out;
    check(!reader.read_dol(8, out), "truncated or out-of-bounds DOL refused");
  }

  int current_verifies = 0, current_reads = 0;
  host::sysgen::Disc current;
  current.verify_retail = [&] { ++current_verifies; return false; };
  current.read = [&](const std::string&, size_t, Bytes& out) { ++current_reads; out = {42}; return true; };
  const auto lazy_path = root / "lazy.iso";
  host::DiscImageReader base(lazy_path.u8string());
  auto selected = host::sysgen::boot_disc("", base, current, 8, fake_dol);
  Bytes out;
  check(!selected.verify_retail() && current_verifies == 1, "without base, current retail verifier unchanged");
  check(selected.read("asset.dat", 3, out) && out == Bytes({42}) && current_reads == 1,
        "without base, current disc reader unchanged");
  selected = host::sysgen::boot_disc(lazy_path.u8string(), base, current, 8, fake_dol);
  // Create the image after constructing the reader AND selecting callbacks: neither opens it.
  write(lazy_path, image);
  check(selected.verify_retail(), "base opens lazily and verifies its own DOL");
  check(selected.read("ASSET.DAT", 4, out) && out == Bytes({'T', 'W', 'O', '!'}), "base supplies its own FST and bytes");
  check(!selected.read("ASSET.DAT", 3, out), "base callback enforces expected size");
  check(current_verifies == 1 && current_reads == 1, "base callbacks never touch current disc");
  host::DiscImageReader invalid(path.u8string());
  selected = host::sysgen::boot_disc(path.u8string(), invalid, current, 8, fake_dol);
  check(!selected.verify_retail(), "invalid base refuses verification without fallback to current disc");
  std::cout << checks << " checks passed\n";
}
