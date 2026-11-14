// Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
#pragma once
#if MU_SQUAD
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

// Costumes and Kirby's colour hats in an online Squad Strike match. Both games trim at the same
// fixed size per fighter (gmSquad_CostumeBytes, carried in the budget identity facts), never the
// local file, so a skin can never split pools or change the trim. The heap then has room for the
// fixed sizes only: a local file over its fighter's size, or one that cannot be opened, loads the
// session's standard copy instead (the disc's own; a mod profile's in its own view). Cosmetics
// never block a match; only a standard copy over its size does, and nothing over its size is
// ever opened.
namespace slippi::squad_costume {

// A disc path as the host keys it: lower case with a leading '/'.
inline std::string key(const char* path) {
  std::string p = path ? path : "";
  for (char& ch : p) ch = (char)std::tolower((unsigned char)ch);
  if (p.empty() || p[0] != '/') p = "/" + p;
  return p;
}

// The disc names the game can open for a name in its tables, as lbFileGetFullName makes them: a
// name with an extension is opened as it is; a base name, with no extension or a bare trailing dot,
// gets "usd" when the game's language is English and "dat" otherwise. Captain Falcon's red costume
// is such a name in the fighter table, "PlCaRe." (ftcaptain.c re_dat), so it opens PlCaRe.usd or
// PlCaRe.dat, never a file called "PlCaRe.". Either can be opened depending on the language each
// player's game runs in, so both are the costume's files. Empty for an empty base name.
inline std::vector<std::string> opened_names(const std::string& name) {
  const size_t dot = name.find('.');
  if (dot != std::string::npos && dot + 1 < name.size()) return {name};
  const std::string base = name.substr(0, dot);
  if (base.empty()) return {};
  return {base + ".usd", base + ".dat"};
}

// One file the game could open: its entry number (-1: none) and length.
struct File { int32_t entry; uint32_t length; };
inline bool fits(const File& file, uint32_t budget) {
  return file.entry >= 0 && file.length != 0 && file.length <= budget;
}

// The session's standard copy of a path. mod: a mod profile's file, not the disc's own.
struct Standard { File file{-1, 0}; bool mod = false; };

// One costume or hat path the identity facts named. twin: the .usd beside a named .dat, kept as a
// bound in case a disc has one; the game opens a named .dat as it is, so a twin the disc lacks
// (every one on the vanilla disc) is never opened and holds.
struct Budget { uint32_t bytes = 0; bool twin = false; };

// The costume and colour hat files the game's identity facts named, with their fixed sizes.
// A fact is [3, use, kind (4), bytes (4), name NUL]; uses 2 (costume) and 3 (hat) are the cosmetics.
// The name is the game's table string; the files are the disc names it opens (opened_names). The
// identity hashes the fact bytes as sent, so this mapping never changes the budget identity.
class Budgets {
 public:
  void clear() { files_.clear(); }
  void record(const uint8_t* fact, uint32_t n) {
    if (!fact || n < 11 || fact[0] != 3 || (fact[1] != 2 && fact[1] != 3)) return;
    const uint32_t bytes = (uint32_t)fact[6] << 24 | (uint32_t)fact[7] << 16 | (uint32_t)fact[8] << 8 | fact[9];
    const char* name = (const char*)fact + 10;
    if (bytes == 0) return;
    for (const std::string& opened : opened_names(std::string(name, strnlen(name, n - 10)))) {
      const std::string path = key(opened.c_str());
      files_[path] = {bytes, false};   // a named file is never a twin, whatever came first
      const size_t dot = path.rfind('.');
      if (dot != std::string::npos && path.compare(dot, std::string::npos, ".dat") == 0)
        files_.emplace(path.substr(0, dot) + ".usd", Budget{bytes, true});
    }
  }
  Budget budget(const std::string& path) const {
    const auto found = files_.find(path);
    return found == files_.end() ? Budget{} : found->second;
  }
  const std::unordered_map<std::string, Budget>& files() const { return files_; }

 private:
  std::unordered_map<std::string, Budget> files_;
};

// Whether a standard copy keeps the trim: it fits, or it is a twin the disc lacks (never opened;
// its .dat has its own standard copy). Anything else refuses the search or the match, including a
// file the game opens that the disc lacks (for "PlCaRe.", either of PlCaRe.usd and PlCaRe.dat).
inline bool standard_holds(const Budget& budget, const File& standard) {
  return fits(standard, budget.bytes) || (budget.twin && standard.entry < 0);
}

// What a lookup opens in an online squad match. Never a file over its size.
enum class Choice {
  Opened,     // what the skins resolved to: it fits (or the path has no budget)
  Standard,   // the standard copy: the opened file is over, empty or missing
  Absent,     // a twin the disc lacks: no file, as on the disc
  Over,       // the standard copy is over too: nothing fits, the match must not start
};
struct Pick { Choice choice; File file; };
inline Pick pick(const Budget& budget, const File& opened, const File& standard) {
  if (budget.bytes == 0 || fits(opened, budget.bytes)) return {Choice::Opened, opened};
  if (fits(standard, budget.bytes)) return {Choice::Standard, standard};
  if (budget.twin && standard.entry < 0) return {Choice::Absent, {-1, 0}};
  return {Choice::Over, {-1, 0}};
}

// The host's standard copy of a path in the current view (source_host.cpp squad_standard_copy).
using Finder = Standard (*)(const std::string& path);

// Every recorded path's standard copy, into `out` when given. False, with the first path in name
// order whose copy does not hold and whether that copy is a mod's, when one does not.
inline bool check_standards(const Budgets& budgets, Finder finder,
                            std::unordered_map<std::string, Standard>* out, std::string* over, bool* mod) {
  over->clear();
  *mod = false;
  for (const auto& file : budgets.files()) {
    const Standard standard = finder(file.first);
    if (out) (*out)[file.first] = standard;
    if (!standard_holds(file.second, standard.file) && (over->empty() || file.first < *over)) {
      *over = file.first;
      *mod = standard.mod;
    }
  }
  return over->empty();
}

// Friends' synced select (a console session) charges costumes and hats at the same fixed sizes as
// an online squad match (the game's squad_fixed_budget), and the main heap has room for those only:
// the floor is the heap every native host boots with. So while squads are in play there the game
// arms the same rule (MU_SQUAD_OP_CONSOLE): a skin over its fighter's size opens the standard copy.
// Facts: the costume and hat facts, laid out as op 3's. Both machines of a session share the
// canonical view, so they read the same copies and give the same answer.
class Console {
 public:
  // OFF: forget the facts and disarm (the game sends them again before its next arm).
  void off() { budgets_.clear(); standards_.clear(); armed_ = false; }
  void record(const uint8_t* fact, uint32_t n) { budgets_.record(fact, n); }
  // ARM: 0 no facts yet (send them, then arm again), 1 armed, 2 the disc's own copy of a path is
  // over its size, 3 a mod's (then nothing is armed and `over` names the path). Without a copy
  // finder there is nothing to fall back on: 1, unarmed, as an online match does. Arming again
  // keeps the copies found first.
  uint8_t arm(Finder finder, std::string* over) {
    over->clear();
    if (armed_) return 1;
    if (budgets_.files().empty()) return 0;
    if (!finder) return 1;
    std::unordered_map<std::string, Standard> standards;
    bool mod = false;
    if (!check_standards(budgets_, finder, &standards, over, &mod)) return mod ? 3 : 2;
    standards_ = std::move(standards);
    armed_ = true;
    return 1;
  }
  // A lookup found nothing within a path's size: keep the facts, open nothing more by the rule.
  void disarm() { standards_.clear(); armed_ = false; }
  bool armed() const { return armed_; }
  // A path's size and standard copy while armed; false for a path with no fact.
  bool rule(const std::string& path, Budget* budget, Standard* standard) const {
    if (!armed_) return false;
    const auto found = standards_.find(path);
    if (found == standards_.end()) return false;
    *budget = budgets_.budget(path);
    *standard = found->second;
    return budget->bytes != 0;
  }

 private:
  Budgets budgets_;
  std::unordered_map<std::string, Standard> standards_;
  bool armed_ = false;
};

}  // namespace slippi::squad_costume
#endif
