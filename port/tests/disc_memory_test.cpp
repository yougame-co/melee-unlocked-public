// disc_memory.h: a double-click finds the disc an earlier run used, the YouGame app's AppContainer
// runs included, best place first, and skips a remembered file that no longer verifies.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "../app/disc_memory.h"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static void write(const fs::path& file, const std::string& text) {
  fs::create_directories(file.parent_path());
  std::ofstream(file, std::ios::binary) << text;
}

int main(int argc, char** argv) {
  CHECK(disc_memory::iso_line("") == "");
  CHECK(disc_memory::iso_line("iso=C:\\Games\\melee.iso\r\n") == "C:\\Games\\melee.iso");
  CHECK(disc_memory::iso_line("language=en\niso=a.iso\nnotesseen=1\niso=b.iso") == "b.iso");
  CHECK(disc_memory::iso_line("lobbyiso=x.iso\n") == "");
  CHECK(disc_memory::yougame_profile("YouGame.Game.0123abcd"));
  CHECK(disc_memory::yougame_profile("yougame.staging.game.0123abcd"));
  CHECK(!disc_memory::yougame_profile("Microsoft.WindowsCalculator_8wekyb3d8bbwe"));
  CHECK(!disc_memory::yougame_profile("YouGame."));

  const fs::path root = fs::absolute(argc > 1 ? fs::path(argv[1]) : fs::path("disc_memory_test_root"));
  std::error_code ec;
  fs::remove_all(root, ec);
  const fs::path local = root / "Local";
  const auto decode = [](const std::string& raw, disc_memory::Encoding) { return fs::u8path(raw); };
  // "Verifies" when the file exists and starts with GALE01, as is_melee_102 checks the header.
  const auto verify = [](const fs::path& disc) {
    std::ifstream f(disc, std::ios::binary);
    char head[6] = {};
    return f.read(head, 6) && std::string(head, 6) == "GALE01";
  };
  // "Unmodified" stands in for mod_scan's whole-disc check: here, a file whose bytes end in VANILLA.
  int full_checks = 0;
  const auto unmodified = [&full_checks](const fs::path& disc) {
    ++full_checks;
    std::ifstream f(disc, std::ios::binary);
    const std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return all.size() >= 7 && all.compare(all.size() - 7, 7, "VANILLA") == 0;
  };
  const fs::path good = root / "discs" / "melee.iso", other = root / "discs" / "other.iso", bad = root / "discs" / "notes.txt";
  const fs::path modded = root / "discs" / "20XX.iso";   // the 1.02 header, other contents
  write(good, "GALE01..VANILLA");
  write(other, "GALE01..VANILLA");
  write(bad, "hello");
  write(modded, "GALE01..20XXTE");

  // Nothing remembered anywhere: nothing found (the caller goes on to melee.iso, then the picker).
  CHECK(disc_memory::first_verified(disc_memory::places(local), decode, verify, unmodified).empty());
  CHECK(disc_memory::places({}).empty());

  // Only the YouGame app has run the game (its AppContainer's launcher.ini): a double-click finds it.
  const fs::path app_ini = local / "Packages" / "yougame.game.0123456789abcdef0123456789abcdef" / "AC" / "MeleeUnlocked" / "launcher.ini";
  write(app_ini, "iso=" + good.u8string() + "\n");
  write(local / "Packages" / "Some.OtherApp_123" / "AC" / "MeleeUnlocked" / "launcher.ini", "iso=" + other.u8string() + "\n");
  {
    const auto list = disc_memory::places(local);
    CHECK(list.size() == 3);   // standalone.ini, launcher.ini, the one YouGame profile
    CHECK(disc_memory::first_verified(list, decode, verify, unmodified) == good);
  }

  // Two app profiles (say staging and production): the one written last wins.
  const fs::path staging_ini = local / "Packages" / "YouGame.Staging.Game.ffff" / "AC" / "MeleeUnlocked" / "launcher.ini";
  write(staging_ini, "iso=" + other.u8string() + "\n");
  fs::last_write_time(app_ini, fs::file_time_type::clock::now() - std::chrono::hours(1));
  CHECK(disc_memory::first_verified(disc_memory::places(local), decode, verify, unmodified) == other);
  // The newest one's disc is gone (moved, deleted): the older one still verifies.
  write(staging_ini, "iso=" + (root / "discs" / "moved.iso").u8string() + "\n");
  CHECK(disc_memory::first_verified(disc_memory::places(local), decode, verify, unmodified) == good);

  // A run outside the app (launcher, .bat, shortcut) comes before the app's runs ...
  write(local / "MeleeUnlocked" / "launcher.ini", "iso=" + other.u8string() + "\n");
  CHECK(disc_memory::first_verified(disc_memory::places(local), decode, verify, unmodified) == other);
  // ... unless what it remembered is not a Melee disc.
  write(local / "MeleeUnlocked" / "launcher.ini", "iso=" + bad.u8string() + "\n");
  CHECK(disc_memory::first_verified(disc_memory::places(local), decode, verify, unmodified) == good);

  // A modded disc with the 1.02 header (a 20XX or Hack Pack ISO) from a mod run's launcher.ini is
  // skipped, and nothing is remembered from it: the app's vanilla disc is taken instead.
  write(local / "MeleeUnlocked" / "launcher.ini", "iso=" + modded.u8string() + "\n");
  CHECK(verify(modded));
  {
    const fs::path standalone = local / "MeleeUnlocked-YouGame" / "standalone.ini";
    const fs::path found = disc_memory::first_verified(disc_memory::places(local), decode, verify, unmodified);
    CHECK(found == good);
    // Every place but the double-click's own pick holds only modded discs: nothing is found, so the
    // launcher goes on to melee.iso and the picker, and standalone.ini is never written.
    write(app_ini, "iso=" + modded.u8string() + "\n");
    write(staging_ini, "iso=" + modded.u8string() + "\n");
    const fs::path none = disc_memory::first_verified(disc_memory::places(local), decode, verify, unmodified);
    CHECK(none.empty());
    if (!none.empty()) disc_memory::remember(standalone, none);
    CHECK(!fs::exists(standalone));
    write(app_ini, "iso=" + good.u8string() + "\n");
  }

  // The double-click's own pick comes first of all, on the header check alone (it passed before).
  write(local / "MeleeUnlocked-YouGame" / "standalone.ini", "iso=" + other.u8string() + "\n");
  {
    const auto list = disc_memory::places(local);
    CHECK(list.front().encoding == disc_memory::Encoding::Utf8);
    CHECK(list[1].encoding == disc_memory::Encoding::Ansi);
    full_checks = 0;
    CHECK(disc_memory::first_verified(list, decode, verify, unmodified) == other);
    CHECK(full_checks == 0);
  }
  // remember() writes the line read back.
  disc_memory::remember(local / "MeleeUnlocked-YouGame" / "standalone.ini", good);
  CHECK(disc_memory::read_iso(local / "MeleeUnlocked-YouGame" / "standalone.ini") == good.u8string());

  fs::remove_all(root, ec);
  if (failures) return 1;
  std::printf("disc_memory: ok\n");
  return 0;
}
