// Synthetic fixtures and shipped manifest. No disc, game process, network or external delta encoder.
#include "sysgen.h"
#include "vcdiff.h"
#include <map>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#ifdef _WIN32
#include <windows.h>
#endif

using Bytes = std::vector<uint8_t>;
using Json = nlohmann::json;
namespace fs = std::filesystem;
static void check(bool ok, const std::string& message) {
  if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
static Bytes bytes(const std::string& s) { return Bytes(s.begin(), s.end()); }
static Bytes read_all(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return Bytes(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}
static void append(Bytes& a, const Bytes& b) { a.insert(a.end(), b.begin(), b.end()); }
static void integer(Bytes& out, size_t n) {
  if (n >= 128) integer(out, n >> 7);
  out.push_back(uint8_t(n & 127) | 128);
}
static void number(Bytes& out, size_t n) { integer(out, n); out.back() &= 127; }
static Bytes header(bool app = false) {
  Bytes d = {0xd6, 0xc3, 0xc4, 0, uint8_t(app ? 4 : 0)};
  if (app) append(d, {3, 'x', 'd', '3'});
  return d;
}
static void window(Bytes& d, uint8_t flags, size_t segment_size, size_t position, size_t length,
                   const Bytes& data, const Bytes& instructions, const Bytes& addresses) {
  d.push_back(flags);
  if (flags & 3) { number(d, segment_size); number(d, position); }
  Bytes body;
  number(body, length); body.push_back(0);
  number(body, data.size()); number(body, instructions.size()); number(body, addresses.size());
  append(body, data); append(body, instructions); append(body, addresses);
  number(d, body.size()); append(d, body);
}
static Bytes add_delta(const Bytes& target) {
  Bytes d = header(); Bytes inst = {1}; number(inst, target.size());
  window(d, 0, 0, 0, target.size(), target, inst, {});
  return d;
}
static void decoder_tests() {
  Bytes out; std::string error;
  const Bytes source = bytes("hello world");
  auto decode = [&](const Bytes& delta) {
    return host::vcdiff_decode(source.data(), source.size(), delta.data(), delta.size(), out, &error);
  };
  Bytes d = header(true);
  window(d, 1, 11, 0, 14, bytes(" there!"), {21, 7, 0, 3}, {0});
  window(d, 1, 5, 6, 5, {}, {21}, {0});
  check(decode(d) && out == bytes("hello there!!!world"), "SOURCE multiwindow ADD RUN COPY: " + error);
  d = header();
  window(d, 0, 0, 0, 10, bytes("ab"), {3, 20, 20}, {0, 2});
  check(decode(d) && out == bytes("ababababab"), "overlapping COPY");
  // Exercise every COPY addressing mode; first copy primes NEAR and SAME at address 1.
  for (unsigned mode = 0; mode <= 8; ++mode) {
    d = header(); Bytes addresses = {1};
    if (mode == 0) append(addresses, {1});
    else if (mode == 1) append(addresses, {14}); // HERE=11+4, address=1
    else if (mode == 2) append(addresses, {0});
    else if (mode <= 5) append(addresses, {1});
    else append(addresses, {1}); // SAME banks 1/2 were zero-initialized
    window(d, 1, 11, 0, 8, {}, {20, uint8_t(20 + 16 * mode)}, addresses);
    check(decode(d), "COPY mode " + std::to_string(mode) + ": " + error);
    const auto expected = mode < 7 ? bytes("elloello") : bytes("ellohell");
    check(out == expected, "COPY address cache content");
  }
  // Paired table instructions: ADD1/COPY4 then COPY4/ADD1.
  d = header(); window(d, 1, 11, 0, 10, bytes("!?"), {163, 247}, {0, 1});
  check(decode(d) && out == bytes("!hellello?"), "paired default table instructions");
  const Bytes target(300, 'a');
  check(decode(add_delta(target)) && out == target, "ADD with multibyte lengths");
  check(decode(header()) && out.empty(), "empty stream");
}
static void write(const fs::path& path, const Bytes& b) {
  fs::create_directories(path.parent_path());
  std::ofstream f(path, std::ios::binary); f.write(reinterpret_cast<const char*>(b.data()), b.size()); f.close();
  check(bool(f), "write fixture");
}
static Json manifest(const Bytes& target, const Bytes& source) {
  return {{"files", Json::array({{{"target", "GameSetup_gui.dat"}, {"sys", {"Sys", "SysPlayback", "slippi_sys_general"}},
      {"target_sha1", host::sysgen::sha1(target)}, {"target_size", target.size()},
      {"sources", Json::array({{{"disc", "GmTou1p.dat"}, {"sha1", host::sysgen::sha1(source)}, {"size", source.size()}}})},
      {"delta", "GameSetup_gui.dat.vcdiff"}}})}};
}
static void manifest_tests() {
  std::vector<host::sysgen::File> files; std::string error;
  const auto valid = manifest(bytes("abc"), bytes("disc"));
  check(host::sysgen::parse_manifest(valid.dump(), files, error) && files.size() == 1, "valid manifest: " + error);
  for (const auto* field : {"target", "sys", "target_sha1", "target_size", "sources", "delta"}) {
    auto j = valid; j["files"][0].erase(field);
    check(!host::sysgen::parse_manifest(j.dump(), files, error) && files.empty(), std::string("missing ") + field);
  }
  for (const auto* field : {"disc", "sha1", "size"}) {
    auto j = valid; j["files"][0]["sources"][0].erase(field);
    check(!host::sysgen::parse_manifest(j.dump(), files, error), std::string("missing source ") + field);
  }
  for (const auto& hash : {std::string(40, 'z'), std::string(39, 'a'), std::string(41, 'a')}) {
    auto j = valid; j["files"][0]["target_sha1"] = hash;
    check(!host::sysgen::parse_manifest(j.dump(), files, error), "bad target hex");
    j = valid; j["files"][0]["sources"][0]["sha1"] = hash;
    check(!host::sysgen::parse_manifest(j.dump(), files, error), "bad source hex");
  }
  auto j = valid; j["files"][0]["target_sha1"] = std::string(40, 'A');
  check(host::sysgen::parse_manifest(j.dump(), files, error) && files[0].sha1 == std::string(40, 'a'), "uppercase hex normalized");
  for (const auto& value : {Json(-1), Json(1.5), Json(uint64_t(1) << 40)}) {
    j = valid; j["files"][0]["target_size"] = value;
    check(!host::sysgen::parse_manifest(j.dump(), files, error), "bad size rejected");
  }
  j = valid; j["files"][0]["delta"] = "../escape";
  check(!host::sysgen::parse_manifest(j.dump(), files, error), "delta path traversal rejected");
  j = valid; j["files"][0]["sys"] = {"../Sys"};
  check(!host::sysgen::parse_manifest(j.dump(), files, error), "Sys path traversal rejected");
  j = valid; j["files"][0]["sources"][0]["disc"] = "../source";
  check(!host::sysgen::parse_manifest(j.dump(), files, error), "source path traversal rejected");
  j = valid; j["files"].push_back(j["files"][0]);
  check(!host::sysgen::parse_manifest(j.dump(), files, error), "duplicate target rejected");
  check(!host::sysgen::parse_manifest("{", files, error), "invalid JSON rejected");
  check(host::sysgen::sha1({}) == "da39a3ee5e6b4b0d3255bfef95601890afd80709", "SHA1 empty vector");
  check(host::sysgen::sha1(bytes("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d", "SHA1 abc");
  check(host::sysgen::sha1(Bytes(1000000, 'a')) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f", "SHA1 multiblock vector");
}
static void shipped_manifest_tests() {
  const fs::path path = SYSGEN_MANIFEST;
  std::ifstream input(path, std::ios::binary);
  check(input.is_open(), "open shipped manifest: " + path.string());
  const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  check(!input.bad(), "read shipped manifest");
  std::vector<host::sysgen::File> files;
  std::string error;
  // Use the boot applier's parser, including its Sys directory allowlist.
  const bool parsed = host::sysgen::parse_manifest(text, files, error);
  check(parsed, "shipped manifest parses with boot parser: " + error);
  std::vector<std::string> targets;
  const auto is_sha1 = [](const std::string& hash) {
    return hash.size() == 40 && std::all_of(hash.begin(), hash.end(), [](char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
  };
  for (const auto& file : files) {
    targets.push_back(file.target);
    check(is_sha1(file.sha1), "shipped target_sha1 is 40 hex: " + file.target);
    for (const auto& source : file.sources)
      check(is_sha1(source.sha1), "shipped source sha1 is 40 hex: " + source.disc);
    const auto delta = path.parent_path() / file.delta;
    check(fs::is_regular_file(delta), "shipped delta exists: " + delta.string());
    check(fs::file_size(delta) > 0, "shipped delta is nonempty: " + delta.string());
  }
  std::sort(targets.begin(), targets.end());
  check(targets == std::vector<std::string>{"GameSetup_gui.dat", "SlippiCSS_gui.dat", "slpCSS.dat"},
        "shipped manifest contains exactly the three required targets");
  // Pinned to upstream v0.9.207 (Slippi 3.7.0, e6478b848): a new upstream Sys file needs new deltas
  // (make_sys_deltas.py --targets-rev <tag>), never a stale rebuild of the old one.
  const std::map<std::string, std::string> upstream = {
      {"GameSetup_gui.dat", "cb78574af8b6ceede7890d4a6a74d61afefdd8df"},
      {"slpCSS.dat", "c70c600663ded2fefe11aeb439f3e61125273651"},
      {"SlippiCSS_gui.dat", "c613efa143e9efb6effeb98aa6f8650243f88977"}};
  for (const auto& file : files)
    check(upstream.count(file.target) && upstream.at(file.target) == file.sha1, "shipped target is upstream 3.7.0's: " + file.target);
}
static void applier_tests(const fs::path& root) {
  const Bytes target = bytes("hello"), source = bytes("disc"), second_source = bytes("extra");
  const auto build = root / "build", data = root / "data";
  const auto sys = build / "Sys";
  host::sysgen::Config config{build, sys, data};
  auto j = manifest(target, source);
  auto save_manifest = [&] { write(build / "deltas/manifest.json", bytes(j.dump())); };
  auto clean_generated = [&] { fs::remove_all(data / "SysGenerated"); };
  host::sysgen::Assets assets; std::string error;
  // Check message choice and counts while leaving elapsed time unconstrained.
  auto built_message = [&](const std::string& targets, size_t folders) {
    const auto& message = assets.status_message();
    const auto prefix = "sysgen: built " + targets + " into " + (data / "SysGenerated").u8string() + " in ";
    const auto suffix = " ms (" + std::to_string(folders) + " Sys folders)";
    check(message.find(prefix) == 0 && message.size() >= suffix.size() &&
          message.compare(message.size() - suffix.size(), suffix.size(), suffix) == 0,
          "built message lists actual targets and folder count: " + message);
  };
  auto cached_message = [&](size_t count) {
    const auto& message = assets.status_message();
    check(message.find("sysgen: already built (" + std::to_string(count) + " files verified in ") == 0 &&
          message.size() >= 4 && message.compare(message.size() - 4, 4, " ms)") == 0,
          "cached message counts verified files: " + message);
  };
  int verifies = 0, reads = 0;
  bool retail = true, readable = true;
  Bytes disc_bytes = source;
  host::sysgen::Disc disc{
    [&] { ++verifies; return retail; },
    [&](const std::string& name, size_t expected, Bytes& out) {
      ++reads;
      if (name == "GmTou2p.dat") { out = second_source; return readable && expected == second_source.size(); }
      out = disc_bytes;
      return readable && name == "GmTou1p.dat" && expected == source.size();
    }};
  for (const auto* folder : {"Sys", "SysPlayback", "slippi_sys_general"}) {
    write(build / folder / "GameFiles/GALE01/GameSetup_gui.dat", target);
    write(build / folder / "GameFiles/GALE01/slpCSS.dat", bytes("staging"));
    write(build / folder / "GameFiles/GALE01/SlippiCSS_gui.dat", bytes("staging"));
  }
  check(assets.prepare(config, disc, error), "staging without manifest: " + error);
  check(assets.status_message().empty(), "no manifest produces no status message");
  check(verifies == 0 && reads == 0 && !fs::exists(data), "staging performs no generation/disc reads");
  save_manifest(); write(build / "deltas/GameSetup_gui.dat.vcdiff", add_delta(target));
  check(assets.prepare(config, disc, error) && reads == 0, "present originals skip manifest generation");
  check(assets.status_message() == "sysgen: shipped files present; nothing to build", "staging message");
  for (const auto* folder : {"Sys", "SysPlayback", "slippi_sys_general"})
    fs::remove(build / folder / "GameFiles/GALE01/GameSetup_gui.dat");
  check(assets.prepare(config, disc, error), "absent originals generate: " + error);
  built_message("GameSetup_gui.dat", 3);
  check(verifies == 1 && reads == 1, "disc verified once, shared decoded output");
  for (const auto* folder : {"Sys", "SysPlayback", "slippi_sys_general"}) {
    const auto generated = data / "SysGenerated" / folder / "GameSetup_gui.dat";
    check(fs::is_regular_file(generated) && assets.lookup(build / folder, "GameSetup_gui.dat") == generated, "generated lookup redirect");
  }
  check(assets.generated_names(sys) == std::vector<std::string>{"GameSetup_gui.dat"}, "preload discovers generated targets");
  check(assets.lookup(sys, "slpCSS.dat") == sys / "GameFiles/GALE01/slpCSS.dat", "shipped file lookup preserved");
  check(assets.prepare(config, disc, error) && reads == 1, "verified cache reused");
  cached_message(3);
  check(assets.lookup(sys / "", "GameSetup_gui.dat") == data / "SysGenerated/Sys/GameSetup_gui.dat",
        "trailing slash lookup redirects");
  // A stale cache (an older build's generated file, or a damaged one) is rebuilt from the disc and replaced.
  write(data / "SysGenerated/Sys/GameSetup_gui.dat", bytes("wrong"));
  check(assets.prepare(config, disc, error) && reads == 2, "stale cache rebuilt from the disc: " + error);
  check(read_all(data / "SysGenerated/Sys/GameSetup_gui.dat") == target, "stale cache replaced with the target");
  check(assets.lookup(sys, "GameSetup_gui.dat") == data / "SysGenerated/Sys/GameSetup_gui.dat", "rebuilt cache redirect");
  clean_generated(); disc_bytes = bytes("oops");
  check(!assets.prepare(config, disc, error) && error.find("disc source size/SHA-1") != std::string::npos, "modified disc fails closed");
  disc_bytes = source; readable = false;
  check(!assets.prepare(config, disc, error), "missing disc source fails closed");
  readable = true; retail = false;
  check(!assets.prepare(config, disc, error), "nonretail disc fails closed");
  retail = true; write(build / "deltas/GameSetup_gui.dat.vcdiff", bytes("bad"));
  check(!assets.prepare(config, disc, error) && error.find("invalid delta") != std::string::npos, "bad delta fails closed");
  write(build / "deltas/GameSetup_gui.dat.vcdiff", add_delta(bytes("wrong")));
  check(!assets.prepare(config, disc, error) && error.find("rebuilt target") != std::string::npos, "bad target hash fails closed");
  write(build / "deltas/GameSetup_gui.dat.vcdiff", add_delta(target));
  // The decoded hash is correct: only the manifest size differs, in either direction.
  for (const auto size : {target.size() - 1, target.size() + 1}) {
    j["files"][0]["target_size"] = size; save_manifest();
    check(!assets.prepare(config, disc, error) &&
          error == "GameSetup_gui.dat: rebuilt target size/SHA-1 mismatch", "target_size mismatch fails closed");
    check(assets.generated_names(sys).empty() && !fs::exists(data / "SysGenerated"),
          "target_size mismatch publishes no asset or redirect");
  }
  j["files"][0]["target_size"] = target.size(); save_manifest();
  auto bad_config = config; bad_config.data_dir.clear();
  check(!assets.prepare(bad_config, disc, error), "no writable data location fails closed");
  bad_config.data_dir = build / "User";
  check(!assets.prepare(bad_config, disc, error), "never writes inside build");
  // The Linux zip's launcher passes --user-dir <build>/User/Slippi: boot generates into the player's own
  // folder instead of failing the start. An explicit --sysgen-data-dir inside the build still fails.
  {
    std::string note;
    auto moved = config;
    const auto player = root / "player-data/User";
    moved.data_dir = host::sysgen::choose_data_dir(build, (build / "User/Slippi").u8string(), "", player, note);
    check(moved.data_dir == player && assets.prepare(moved, disc, error) &&
          fs::is_regular_file(player / "SysGenerated/Sys/GameSetup_gui.dat") &&
          assets.lookup(sys, "GameSetup_gui.dat") == player / "SysGenerated/Sys/GameSetup_gui.dat",
          "user dir inside the build generates into the player's folder: " + error);
    check(!fs::exists(build / "User"), "nothing written inside the build");
    moved.data_dir = host::sysgen::choose_data_dir(build, (build / "User/Slippi").u8string(),
                                                   (build / "User").u8string(), player, note);
    check(moved.data_dir == build / "User" && note.empty() && !assets.prepare(moved, disc, error) &&
          error.find("generated data must be outside the build directory") != std::string::npos,
          "explicit --sysgen-data-dir inside the build still fails: " + error);
    fs::remove_all(root / "player-data");
  }
  write(root / "blocked-data", bytes("file, not directory"));
  bad_config.data_dir = root / "blocked-data";
  check(!assets.prepare(bad_config, disc, error) && error.find("atomically write") != std::string::npos,
        "write failure fails closed");
  // Two source files concatenate in manifest order.
  j["files"][0]["sources"].push_back({{"disc", "GmTou2p.dat"},
      {"sha1", host::sysgen::sha1(second_source)}, {"size", second_source.size()}});
  save_manifest(); Bytes d = header(); window(d, 1, 9, 0, 5, {}, {21}, {2});
  j["files"][0]["target_sha1"] = host::sysgen::sha1(bytes("scext")); save_manifest();
  write(build / "deltas/GameSetup_gui.dat.vcdiff", d);
  check(assets.prepare(config, disc, error), "ordered concatenated disc sources: " + error);
  // Both absent assets are required, including the flat Sys layout and an independent second delta.
  clean_generated();
  for (const auto* folder : {"Sys", "SysPlayback", "slippi_sys_general"})
    fs::remove(build / folder / "GameFiles/GALE01/slpCSS.dat");
  auto second = j["files"][0];
  second["target"] = "slpCSS.dat"; second["delta"] = "slpCSS.dat.vcdiff";
  second["target_sha1"] = host::sysgen::sha1(target);
  j["files"].push_back(second); save_manifest();
  write(build / "deltas/slpCSS.dat.vcdiff", add_delta(target));
  check(assets.prepare(config, disc, error), "both absent targets generate: " + error);
  check(assets.generated_names(sys).size() == 2, "both generated names preload");
  built_message("GameSetup_gui.dat, slpCSS.dat", 3);
  check(assets.prepare(config, disc, error), "both targets cached: " + error);
  cached_message(6);
  fs::remove(data / "SysGenerated/Sys/slpCSS.dat");
  check(assets.prepare(config, disc, error), "one missing output rebuilt: " + error);
  built_message("slpCSS.dat", 1);
  clean_generated();
  write(sys / "slpCSS.dat", target);
  check(assets.prepare(config, disc, error) && assets.lookup(sys, "slpCSS.dat") == sys / "slpCSS.dat",
        "flat shipped Sys asset skips generation");
  clean_generated(); fs::remove(build / "deltas/manifest.json");
  check(!assets.prepare(config, disc, error), "missing manifest fails closed");
  j["files"][0]["sys"] = {"SysPlayback"}; save_manifest();
  check(!assets.prepare(config, disc, error), "omitted active directory fails closed");
}
// Which folder boot hands prepare: explicit wins, a --user-dir parent inside the game folder (or no
// --user-dir) moves to the player's own folder, one outside stays.
static void data_dir_tests(const fs::path& root) {
  const auto build = root / "pick/build", player = root / "pick/player/User";
  fs::create_directories(build / "Sys");
  std::string note;
  auto pick = [&](const fs::path& user, const std::string& data, const fs::path& fallback) {
    return host::sysgen::choose_data_dir(build, user.empty() ? std::string() : user.u8string(), data, fallback, note);
  };
  check(pick(build / "User/Slippi", "", player) == player &&
        note == "sys: " + (build / "User").lexically_normal().u8string() + " is inside the game folder; generating into " + player.u8string(),
        "user dir inside the build moves to the player's folder: " + note);
  check(pick(build / "User/Slippi/", "", player) == player, "trailing slash inside the build moves too");
  check(pick(build / "Slippi", "", player) == player, "user dir directly in the build: its parent is the build");
  check(pick(root / "pick/home/User/Slippi", "", player) == root / "pick/home/User" && note.empty(),
        "user dir outside the build keeps its parent");
  check(pick({}, (build / "User").u8string(), player) == build / "User" && note.empty(),
        "explicit data dir is never moved, even inside the build");
  check(pick(build / "User/Slippi", (root / "pick/elsewhere").u8string(), player) == root / "pick/elsewhere",
        "explicit data dir wins over user dir");
  check(pick({}, "", player) == player && note == "sys: no --user-dir given; generating into " + player.u8string(),
        "no user dir and no data dir uses the player's folder: " + note);
  check(pick(build / "User/Slippi", "", {}) == build / "User" && note.empty(),
        "no player folder keeps the old answer for prepare to refuse");
  check(pick({}, "", {}).empty(), "nothing at all stays empty");
  // The per-OS folder itself.
#ifdef _WIN32
  SetEnvironmentVariableW(L"LOCALAPPDATA", L"C:\\Users\\p\\AppData\\Local");
  check(host::sysgen::user_data_dir() == fs::path(L"C:\\Users\\p\\AppData\\Local\\MeleeUnlocked-YouGame\\User"),
        "Windows folder is LOCALAPPDATA's MeleeUnlocked-YouGame\\User");
#else
#ifdef __APPLE__
  const std::string name = "melee-unlocked";
#else
  const std::string name = "MeleeUnlocked-YouGame";
#endif
  const char* old_xdg = std::getenv("XDG_DATA_HOME");
  const char* old_home = std::getenv("HOME");
  const std::string saved_xdg = old_xdg ? old_xdg : "", saved_home = old_home ? old_home : "";
  setenv("HOME", "/home/p", 1);
  setenv("XDG_DATA_HOME", "/data/p", 1);
  check(host::sysgen::user_data_dir() == fs::path("/data/p") / name / "User", "XDG_DATA_HOME folder");
  setenv("XDG_DATA_HOME", "relative", 1);
  check(host::sysgen::user_data_dir() == fs::path("/home/p/.local/share") / name / "User", "relative XDG_DATA_HOME ignored");
  unsetenv("XDG_DATA_HOME");
  check(host::sysgen::user_data_dir() == fs::path("/home/p/.local/share") / name / "User", "HOME's .local/share folder");
  unsetenv("HOME");
  check(host::sysgen::user_data_dir().empty(), "no HOME and no XDG_DATA_HOME gives no folder");
  if (old_home) setenv("HOME", saved_home.c_str(), 1);
  if (old_xdg) setenv("XDG_DATA_HOME", saved_xdg.c_str(), 1);
#endif
}
int main() {
  const auto root = fs::temp_directory_path() / ("mu-sysgen-test-" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  struct Cleanup { fs::path path; ~Cleanup() { std::error_code ec; fs::remove_all(path, ec); } } cleanup{root};
  decoder_tests(); manifest_tests(); shipped_manifest_tests(); applier_tests(root); data_dir_tests(root);
  std::cout << "Sys generation: decoder, manifest, verification, atomic output and redirects passed\n";
}
