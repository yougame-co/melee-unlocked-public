// Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
// The online squad costume rule against a stub of the host's file lookup: a disc table, a skin
// layer over it, the identity facts the game sends, op 5's check of the standard copies, and the
// open h_disc_entrynum does with them.
#include "squad_costume_rule.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#if MU_SQUAD
using slippi::squad_costume::Budget;
using slippi::squad_costume::Budgets;
using slippi::squad_costume::Choice;
using slippi::squad_costume::File;
static unsigned checks;
#define CHECK(x) do { ++checks; assert(x); } while (0)

// The game's identity fact: [3, use, kind (4), bytes (4), name NUL] (budget_fact).
static std::vector<uint8_t> fact(uint8_t use, uint32_t kind, uint32_t bytes, const char* name) {
  std::vector<uint8_t> p{3, use, uint8_t(kind >> 24), uint8_t(kind >> 16), uint8_t(kind >> 8), uint8_t(kind),
                         uint8_t(bytes >> 24), uint8_t(bytes >> 16), uint8_t(bytes >> 8), uint8_t(bytes)};
  for (const char* c = name; *c; ++c) p.push_back((uint8_t)*c);
  p.push_back(0);
  return p;
}

// Stub host: the disc's own table (entry -> length), the paths the skins resolved to, and the
// standard copy of each path. arm() is op 5: it checks every standard copy in the match's view and
// freezes them, or refuses the match; disarm() is everything that clears it (op 0, a normal op 5,
// the connection ending). open() is h_disc_entrynum.
struct Host {
  std::vector<uint32_t> lengths;              // entry -> length (0: unreadable)
  std::map<std::string, int32_t> opened;      // what the skin layer resolves a path to
  std::map<std::string, int32_t> disc;        // the disc's own entry for each path
  Budgets budgets;
  bool armed = false;
  std::map<std::string, File> standards;      // frozen at arm()
  unsigned fallbacks = 0, refusals = 0;
  File file(int32_t entry) const {
    return {entry, entry > 0 && entry < (int32_t)lengths.size() ? lengths[(size_t)entry] : 0};
  }
  File standard(const std::string& p) const {
    const auto base = disc.find(p);
    return file(base == disc.end() ? -1 : base->second);
  }
  // squad_standards: the first path in name order whose standard copy does not hold, or "".
  std::string over() const {
    std::string first;
    for (const auto& f : budgets.files())
      if (!slippi::squad_costume::standard_holds(f.second, standard(f.first)) && (first.empty() || f.first < first))
        first = f.first;
    return first;
  }
  bool holds() const { return over().empty(); }   // op 4's answer: 1 when every standard copy holds
  bool arm() {
    disarm();
    std::map<std::string, File> found;
    for (const auto& f : budgets.files()) {
      found[f.first] = standard(f.first);
      if (!slippi::squad_costume::standard_holds(f.second, found[f.first])) return false;
    }
    standards = found;
    return armed = true;
  }
  void disarm() { armed = false; standards.clear(); }
  int32_t open(const char* path) {
    const std::string p = slippi::squad_costume::key(path);
    const auto it = opened.find(p);
    const int32_t entry = it == opened.end() ? -1 : it->second;
    const auto frozen = standards.find(p);
    if (!armed || frozen == standards.end()) return entry;
    const auto picked = slippi::squad_costume::pick(budgets.budget(p), file(entry), frozen->second);
    if (picked.choice == Choice::Over) { ++refusals; return entry; }
    if (picked.choice != Choice::Opened && picked.file.entry != entry) { ++fallbacks; return picked.file.entry; }
    return entry;
  }
};

// The vanilla NTSC 1.02 disc's costume and Kirby colour hat files (GALE01 rev 2 file table): every
// Pl<fighter><colour>.dat/.usd and PlKb<colour>Cp<fighter>.dat with its length. The only .usd is
// PlCaRe.usd, beside PlCaRe.dat.
struct VanillaFile { const char* name; uint32_t length; };
static const VanillaFile kVanilla[] = {
    {"PlBoNr.dat", 79135}, {"PlCaBu.dat", 525771}, {"PlCaGr.dat", 525739}, {"PlCaGy.dat", 525707},
    {"PlCaNr.dat", 525805}, {"PlCaRe.dat", 525771}, {"PlCaRe.usd", 558539}, {"PlCaWh.dat", 526027},
    {"PlChNr.dat", 313513}, {"PlClBk.dat", 452050}, {"PlClBu.dat", 452050}, {"PlClNr.dat", 452046},
    {"PlClRe.dat", 452050}, {"PlClWh.dat", 452050}, {"PlDkBk.dat", 641744}, {"PlDkBu.dat", 641744},
    {"PlDkGr.dat", 641744}, {"PlDkNr.dat", 641740}, {"PlDkRe.dat", 641744}, {"PlDrBk.dat", 483018},
    {"PlDrBu.dat", 483018}, {"PlDrGr.dat", 483018}, {"PlDrNr.dat", 483014}, {"PlDrRe.dat", 483018},
    {"PlFcBu.dat", 243998}, {"PlFcGr.dat", 243998}, {"PlFcNr.dat", 243994}, {"PlFcRe.dat", 243998},
    {"PlFeBu.dat", 660288}, {"PlFeGr.dat", 660288}, {"PlFeNr.dat", 660284}, {"PlFeRe.dat", 660288},
    {"PlFeYe.dat", 660288}, {"PlFxGr.dat", 362982}, {"PlFxLa.dat", 362982}, {"PlFxNr.dat", 362978},
    {"PlFxOr.dat", 362982}, {"PlGkNr.dat", 515320}, {"PlGlNr.dat", 84444}, {"PlGnBu.dat", 391121},
    {"PlGnGr.dat", 391121}, {"PlGnLa.dat", 391121}, {"PlGnNr.dat", 387979}, {"PlGnRe.dat", 391121},
    {"PlGwNr.dat", 113895}, {"PlKbBu.dat", 283774}, {"PlKbBuCpDk.dat", 181734}, {"PlKbBuCpFc.dat", 169746},
    {"PlKbBuCpMt.dat", 170478}, {"PlKbBuCpPr.dat", 185178}, {"PlKbGr.dat", 283710},
    {"PlKbGrCpDk.dat", 181254}, {"PlKbGrCpFc.dat", 186678}, {"PlKbGrCpMt.dat", 170414},
    {"PlKbGrCpPr.dat", 185114}, {"PlKbNr.dat", 283994}, {"PlKbNrCpDk.dat", 181538},
    {"PlKbNrCpFc.dat", 186674}, {"PlKbNrCpGw.dat", 108938}, {"PlKbNrCpMt.dat", 171230},
    {"PlKbNrCpPr.dat", 185398}, {"PlKbRe.dat", 283742}, {"PlKbReCpDk.dat", 181286},
    {"PlKbReCpFc.dat", 186678}, {"PlKbReCpMt.dat", 170446}, {"PlKbReCpPr.dat", 185146},
    {"PlKbWh.dat", 283934}, {"PlKbWhCpDk.dat", 181478}, {"PlKbWhCpFc.dat", 186678},
    {"PlKbWhCpMt.dat", 170638}, {"PlKbWhCpPr.dat", 185338}, {"PlKbYe.dat", 283678},
    {"PlKbYeCpDk.dat", 181222}, {"PlKbYeCpFc.dat", 186678}, {"PlKbYeCpMt.dat", 170382},
    {"PlKbYeCpPr.dat", 185082}, {"PlKpBk.dat", 493586}, {"PlKpBu.dat", 493650}, {"PlKpNr.dat", 518222},
    {"PlKpRe.dat", 493522}, {"PlLgAq.dat", 373098}, {"PlLgNr.dat", 373094}, {"PlLgPi.dat", 373098},
    {"PlLgWh.dat", 487174}, {"PlLkBk.dat", 491576}, {"PlLkBu.dat", 491576}, {"PlLkNr.dat", 491572},
    {"PlLkRe.dat", 491576}, {"PlLkWh.dat", 491576}, {"PlMhNr.dat", 313338}, {"PlMrBk.dat", 473526},
    {"PlMrBu.dat", 473526}, {"PlMrGr.dat", 473526}, {"PlMrNr.dat", 473522}, {"PlMrYe.dat", 473526},
    {"PlMsBk.dat", 594964}, {"PlMsGr.dat", 594964}, {"PlMsNr.dat", 594960}, {"PlMsRe.dat", 594964},
    {"PlMsWh.dat", 594964}, {"PlMtBu.dat", 255156}, {"PlMtGr.dat", 255156}, {"PlMtNr.dat", 263344},
    {"PlMtRe.dat", 255156}, {"PlNnAq.dat", 350684}, {"PlNnNr.dat", 350748}, {"PlNnWh.dat", 350684},
    {"PlNnYe.dat", 350720}, {"PlNsBu.dat", 375368}, {"PlNsGr.dat", 375368}, {"PlNsNr.dat", 373256},
    {"PlNsYe.dat", 375336}, {"PlPcBu.dat", 230146}, {"PlPcGr.dat", 316154}, {"PlPcNr.dat", 168974},
    {"PlPcRe.dat", 206334}, {"PlPeBu.dat", 806990}, {"PlPeGr.dat", 806862}, {"PlPeNr.dat", 809794},
    {"PlPeWh.dat", 806798}, {"PlPeYe.dat", 764870}, {"PlPkBu.dat", 158102}, {"PlPkGr.dat", 159922},
    {"PlPkNr.dat", 135634}, {"PlPkRe.dat", 166982}, {"PlPpGr.dat", 350624}, {"PlPpNr.dat", 350620},
    {"PlPpOr.dat", 350620}, {"PlPpRe.dat", 350744}, {"PlPrBu.dat", 258435}, {"PlPrGr.dat", 255987},
    {"PlPrNr.dat", 244518}, {"PlPrRe.dat", 254695}, {"PlPrYe.dat", 264147}, {"PlSbNr.dat", 47106},
    {"PlSkBu.dat", 380180}, {"PlSkGr.dat", 380084}, {"PlSkNr.dat", 380176}, {"PlSkRe.dat", 380020},
    {"PlSkWh.dat", 380180}, {"PlSsBk.dat", 401741}, {"PlSsGr.dat", 407885}, {"PlSsLa.dat", 401741},
    {"PlSsNr.dat", 401739}, {"PlSsPi.dat", 407885}, {"PlYsAq.dat", 295166}, {"PlYsBu.dat", 294942},
    {"PlYsNr.dat", 294834}, {"PlYsPi.dat", 295198}, {"PlYsRe.dat", 295258}, {"PlYsYe.dat", 295166},
    {"PlZdBu.dat", 345374}, {"PlZdGr.dat", 345374}, {"PlZdNr.dat", 350082}, {"PlZdRe.dat", 345374},
    {"PlZdWh.dat", 349214}
};

static void vanilla_disc() {
  // The disc as a file table: entry i + 1 is kVanilla[i]. No skins.
  Host h;
  h.lengths.push_back(0);
  std::map<std::string, uint32_t> worst, hats;   // the constants: each fighter's largest
  for (const auto& f : kVanilla) {
    const std::string name = f.name;
    h.disc[slippi::squad_costume::key(f.name)] = (int32_t)h.lengths.size();
    h.lengths.push_back(f.length);
    if (name.compare(0, 4, "PlKb") == 0 && name.size() == 14 && name.compare(6, 2, "Cp") == 0)
      hats[name.substr(8, 2)] = std::max(hats[name.substr(8, 2)], f.length);
    else worst[name.substr(2, 2)] = std::max(worst[name.substr(2, 2)], f.length);
  }
  h.opened = h.disc;
  // The names the game sends are its own tables' (mu_squad_fighter_files, mu_squad_copy_files):
  // every costume and hat by its .dat, except Captain Falcon's red costume, whose name in the
  // fighter table is the base name "PlCaRe." (ftcaptain.c re_dat): lbFileGetFullName opens it as
  // PlCaRe.usd when the game's language is English and PlCaRe.dat otherwise.
  unsigned named = 0, twins_absent = 0;
  for (const auto& f : kVanilla) {
    const std::string name = f.name;
    if (name.size() < 4 || name.compare(name.size() - 4, 4, ".dat") != 0) continue;
    const bool hat = name.compare(0, 4, "PlKb") == 0 && name.size() == 14 && name.compare(6, 2, "Cp") == 0;
    const char* sent = name == "PlCaRe.dat" ? "PlCaRe." : f.name;
    const auto made = fact(hat ? 3 : 2, 0, hat ? hats[name.substr(8, 2)] : worst[name.substr(2, 2)], sent);
    h.budgets.record(made.data(), (uint32_t)made.size());
    ++named;
  }
  for (const auto& f : h.budgets.files())
    if (f.second.twin && !h.disc.count(f.first)) ++twins_absent;
  CHECK(named == 155 && h.budgets.files().size() == 2 * named);   // every .dat and its .usd
  CHECK(twins_absent == named - 1);                                // all but Falcon red's pair
  CHECK(!h.budgets.files().count("/plcare."));                     // never a path of its own
  CHECK(!h.budgets.budget("/plcare.usd").twin && h.budgets.budget("/plcare.usd").bytes == 558539);
  CHECK(!h.budgets.budget("/plcare.dat").twin && h.budgets.budget("/plcare.dat").bytes == 558539);
  // A clean disc: the search is taken (op 4 answers 1) and the match is armed (op 5).
  CHECK(h.over().empty() && h.holds() && h.arm());
  // Every lookup is the disc's own file; the English twins the disc lacks stay absent;
  // PlCaRe.usd is its own file.
  for (const auto& f : h.disc) CHECK(h.open(f.first.c_str()) == f.second);
  CHECK(h.open("PlFxGr.usd") == -1 && h.open("PlCaRe.usd") == h.disc["/plcare.usd"]);
  CHECK(!h.fallbacks && !h.refusals);
  // A skin pack over every costume and hat: each loads the disc's own copy in the match.
  const int32_t big = (int32_t)h.lengths.size();
  h.lengths.push_back(900000);
  for (auto& o : h.opened) o.second = big;
  for (const auto& f : h.disc) CHECK(h.open(f.first.c_str()) == f.second);
  CHECK(h.fallbacks == h.disc.size() && !h.refusals);
  // A Blood Falcon skin over the size: both of Falcon red's names load the disc's own copy.
  CHECK(h.open("PlCaRe.usd") == h.disc["/plcare.usd"] && h.open("PlCaRe.dat") == h.disc["/plcare.dat"]);
  // An oversized English skin the disc has no twin for: absent in the match, so the game opens the
  // .dat (and that falls back on its own), the skin outside one.
  h.opened["/plfxgr.usd"] = big;
  CHECK(h.open("PlFxGr.usd") == -1);
  h.disarm();
  CHECK(h.open("PlFxGr.usd") == big && h.open("PlFxGr.dat") == big);
  std::printf("vanilla disc: %u costumes and hats, %u twins absent, every standard copy holds\n", named, twins_absent);
}

int main() {
  CHECK(slippi::squad_costume::key("PlFxGr.dat") == "/plfxgr.dat");
  CHECK(slippi::squad_costume::key("/PlCaRe.USD") == "/plcare.usd");
  Host h;
  // Disc: 1 Fox green (vanilla), 2 Falcon red, 3 Falcon red English, 4 Kirby's Falco hat, 5 Fox's data.
  h.lengths = {0, 362982, 525771, 558539, 186678, 600000,
               // skins: 6 Fox green 400 KB, 7 Fox green 200 KB, 8 an empty skin, 9 Falcon red skin
               400000, 200000, 0, 558000};
  h.disc = {{"/plfxgr.dat", 1}, {"/plcare.dat", 2}, {"/plcare.usd", 3}, {"/plkbgrcpfc.dat", 4}, {"/plfx.dat", 5}};
  h.opened = h.disc;
  // The identity facts: costumes (2) and hats (3) at their fixed sizes, others never recorded.
  for (const auto& f : {fact(2, 1, 362982, "PlFxGr.dat"), fact(2, 2, 558539, "PlCaRe."),
                        fact(3, 22, 186678, "PlKbGrCpFc.dat"), fact(0, 5, 600000, "PlFx.dat"),
                        fact(1, 5, 1000, "PlFxAJ.dat")}) {
    h.budgets.record(f.data(), (uint32_t)f.size());
  }
  CHECK(h.budgets.budget("/plfxgr.dat").bytes == 362982 && !h.budgets.budget("/plfxgr.dat").twin);
  CHECK(h.budgets.budget("/plfxgr.usd").bytes == 362982 && h.budgets.budget("/plfxgr.usd").twin);
  // Falcon red's base name: both files the game can open carry the size, the bare name none.
  CHECK(h.budgets.budget("/plcare.usd").bytes == 558539 && !h.budgets.budget("/plcare.usd").twin);
  CHECK(h.budgets.budget("/plcare.dat").bytes == 558539 && !h.budgets.budget("/plcare.dat").twin);
  CHECK(!h.budgets.budget("/plcare.").bytes && !h.budgets.files().count("/plcare."));
  CHECK(h.budgets.budget("/plkbgrcpfc.dat").bytes == 186678);
  CHECK(!h.budgets.budget("/plfx.dat").bytes && !h.budgets.budget("/plfxaj.dat").bytes);
  {
    Budgets bad;
    const auto truncated = fact(2, 1, 362982, "PlFxGr.dat");
    bad.record(truncated.data(), 10);   // no name
    const auto zero = fact(2, 1, 0, "PlFxGr.dat");
    bad.record(zero.data(), (uint32_t)zero.size());
    bad.record(nullptr, 20);
    CHECK(bad.files().empty());
    // A name the game sends itself is never a twin, whichever fact came first.
    const auto dat = fact(2, 1, 1000, "PlXxYy.dat"), usd = fact(2, 1, 2000, "PlXxYy.usd");
    bad.record(dat.data(), (uint32_t)dat.size());
    CHECK(bad.budget("/plxxyy.usd").twin);
    bad.record(usd.data(), (uint32_t)usd.size());
    CHECK(!bad.budget("/plxxyy.usd").twin && bad.budget("/plxxyy.usd").bytes == 2000);
    bad.record(dat.data(), (uint32_t)dat.size());
    CHECK(!bad.budget("/plxxyy.usd").twin);
  }
  {
    // lbFileGetFullName, rule for rule: an extension is kept; a base name with no extension or a
    // bare trailing dot opens as .usd (English) or .dat; an empty name is nothing.
    Budgets names;
    for (const auto& f : {fact(2, 1, 100, "PlXxAa"), fact(2, 1, 200, "PlXxBb."), fact(2, 1, 300, "PlXxCc.usd"),
                          fact(2, 1, 400, "PlXxDd.lat"), fact(2, 1, 500, ""), fact(2, 1, 600, ".")})
      names.record(f.data(), (uint32_t)f.size());
    CHECK(names.budget("/plxxaa.usd").bytes == 100 && names.budget("/plxxaa.dat").bytes == 100);
    CHECK(!names.budget("/plxxaa.usd").twin && !names.budget("/plxxaa.dat").twin);
    CHECK(names.budget("/plxxbb.usd").bytes == 200 && names.budget("/plxxbb.dat").bytes == 200);
    CHECK(!names.files().count("/plxxaa") && !names.files().count("/plxxbb."));
    CHECK(names.budget("/plxxcc.usd").bytes == 300 && !names.files().count("/plxxcc.dat"));
    CHECK(names.budget("/plxxdd.lat").bytes == 400 && names.files().size() == 6);
  }
  // The rule is off until op 5 arms it: the CSS and normal play open the skins.
  h.opened["/plfxgr.dat"] = 6;
  CHECK(h.open("PlFxGr.dat") == 6 && !h.fallbacks);
  // Vanilla everywhere: nothing changes in a squad match. PlFxGr.usd is a twin the disc lacks.
  h.opened["/plfxgr.dat"] = 1;
  CHECK(h.holds() && h.arm());
  CHECK(h.open("PlFxGr.dat") == 1 && h.open("PlCaRe.usd") == 3 && h.open("PlCaRe.dat") == 2 &&
        h.open("PlFx.dat") == 5 && !h.fallbacks);
  CHECK(h.open("PlFxGr.usd") == -1 && !h.fallbacks);
  // An oversized skin: the disc's copy during a squad match, the skin outside one.
  h.opened["/plfxgr.dat"] = 6;
  CHECK(h.open("PlFxGr.dat") == 1 && h.fallbacks == 1);
  h.disarm();
  CHECK(h.open("PlFxGr.dat") == 6 && h.fallbacks == 1);
  // A skin within its size keeps itself in a squad match.
  CHECK(h.arm());
  h.opened["/plfxgr.dat"] = 7;
  CHECK(h.open("PlFxGr.dat") == 7 && h.fallbacks == 1);
  // An empty or missing skin falls back to the disc's copy.
  h.opened["/plfxgr.dat"] = 8;
  CHECK(h.open("PlFxGr.dat") == 1 && h.fallbacks == 2);
  h.opened.erase("/plfxgr.dat");
  CHECK(h.open("/PLFXGR.DAT") == 1 && h.fallbacks == 3);
  // An English skin with no twin on the disc: kept when it fits, absent (the .dat opens) when over.
  h.opened["/plfxgr.usd"] = 7;
  CHECK(h.open("PlFxGr.usd") == 7 && h.fallbacks == 3);
  h.opened["/plfxgr.usd"] = 6;
  CHECK(h.open("PlFxGr.usd") == -1 && h.fallbacks == 4);
  // Falcon red's English copy at its fighter's size; a skin of it just under keeps itself.
  h.opened["/plcare.usd"] = 9;
  CHECK(h.open("PlCaRe.usd") == 9 && h.fallbacks == 4);
  // Kirby's colour hat follows the same rule.
  h.opened["/plkbgrcpfc.dat"] = 6;
  CHECK(h.open("PlKbGrCpFc.dat") == 4 && h.fallbacks == 5);
  // A non-costume file is never touched, whatever its length.
  h.lengths[5] = 900000;
  CHECK(h.open("PlFx.dat") == 5 && h.fallbacks == 5);
  // A standard copy over its size (a modded base disc): op 4 refuses the search and op 5 the match,
  // and pick never returns a file over its size.
  h.lengths[4] = 190000;
  CHECK(!h.holds() && !h.arm() && !h.armed);
  CHECK(h.open("PlKbGrCpFc.dat") == 6);   // not armed: the lookup is as it was
  {
    const Budget hat{186678, false};
    const auto none = slippi::squad_costume::pick(hat, File{6, 400000}, File{4, 190000});
    CHECK(none.choice == Choice::Over && none.file.entry == -1);
    const auto missing = slippi::squad_costume::pick(hat, File{6, 400000}, File{-1, 0});
    CHECK(missing.choice == Choice::Over && missing.file.entry == -1);   // only twins may be absent
  }
  h.lengths[4] = 186678;
  // Falcon red's English copy over its fighter's size (a modded base disc) refuses too, by the name
  // the game opens; so does a disc without the Japanese one.
  h.lengths[3] = 558540;
  CHECK(h.over() == "/plcare.usd" && !h.arm());
  h.lengths[3] = 558539;
  h.disc.erase("/plcare.dat");
  CHECK(h.over() == "/plcare.dat" && !h.arm());
  h.disc["/plcare.dat"] = 2;
  CHECK(h.holds());
  // The copies are frozen at op 5: a later change to the disc table does not reach the match.
  CHECK(h.arm());
  h.lengths[1] = 400000;
  h.opened["/plfxgr.dat"] = 6;
  CHECK(h.open("PlFxGr.dat") == 1);
  h.lengths[1] = 362982;
  // A named path the disc lacks (not a twin) has no standard copy: refused.
  const auto orphan = fact(2, 1, 362982, "PlFxNr.dat");
  h.budgets.record(orphan.data(), (uint32_t)orphan.size());
  CHECK(!h.holds() && !h.arm());
  CHECK(!slippi::squad_costume::fits(File{-1, 0}, 362982));
  CHECK(!slippi::squad_costume::fits(File{1, 362983}, 362982) && slippi::squad_costume::fits(File{1, 362982}, 362982));
  vanilla_disc();
  std::printf("squad costume rule: PASS (%u checks)\n", checks);
  return 0;
}
#else
int main() { std::puts("squad costume rule: PASS (squad off)"); return 0; }
#endif
