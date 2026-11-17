// pipeline_cache_store.h and progress_frame.h: saves happen off the caller's thread, are atomic,
// flush_all reaches every live saver and reports failed writes, holds and freezes stop timed saves
// and every save, and the progress frame stays inside its target. prewarm_variants.h: a start waits
// only for the pipelines this session draws with, so a second start never waits again.
#include "pipeline_cache_store.h"
#include "progress_frame.h"
#include "prewarm_variants.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#define check(x) do { if (!(x)) { std::printf("pipeline_cache_store_test FAILED line %d: %s\n", __LINE__, #x); std::fflush(stdout); std::abort(); } } while (0)

namespace fs = std::filesystem;
using namespace std::chrono_literals;
namespace cs = gx::cache_store;

static std::string slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}
static size_t temporaries(const fs::path& dir) {
  size_t n = 0;
  for (auto& e : fs::directory_iterator(dir)) if (e.path().extension() == ".tmp") ++n;
  return n;
}

int main(int argc, char** argv) {
  const fs::path dir = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "pipeline_cache_store_test";
  fs::remove_all(dir);
  fs::create_directories(dir);

  // Atomic write: replaces the old file whole and leaves no temporary behind.
  {
    const fs::path p = dir / "a.bin";
    check(cs::write_file_atomic(p.string(), "old contents", 12));
    check(cs::write_file_atomic(p.string(), "new", 3));
    check(slurp(p) == "new");
    check(temporaries(dir) == 0);
    check(!cs::write_file_atomic((dir / "missing" / "x.bin").string(), "x", 1));   // no folder: refused, nothing left
    check(temporaries(dir) == 0);
  }

  // A saver writes on its own thread after the quiet period, and the producer never runs on the
  // thread that marked it dirty.
  {
    std::atomic<int> produced{0};
    std::atomic<bool> other_thread{true};
    std::atomic<uint64_t> logged{0};
    const auto caller = std::this_thread::get_id();
    std::atomic<int> generation{0};
    cs::Saver saver(50ms, 200ms);
    saver.add((dir / "lib.bin").string(), [&](cs::Buffer& out) {
      ++produced;
      if (std::this_thread::get_id() == caller) other_thread = false;
      const std::string s = "gen" + std::to_string(generation.load());
      out.assign(s.data(), s.size());
      return cs::Produce::Write;
    });
    saver.on_saved([&](uint64_t bytes, const char*) { logged += bytes; });
    saver.start();
    check(produced == 0);   // nothing dirty, nothing written
    generation = 1;
    saver.mark_dirty();
    for (int i = 0; i < 400 && saver.saves() == 0; ++i) std::this_thread::sleep_for(5ms);   // written, not just produced
    check(produced == 1);
    check(other_thread);
    check(slurp(dir / "lib.bin") == "gen1");
    check(logged == 4 && saver.saved_bytes() == 4);

    // Marks that keep coming still save within the longest wait.
    const auto t0 = std::chrono::steady_clock::now();
    while (saver.saves() == 1 && std::chrono::steady_clock::now() - t0 < 2s) { saver.mark_dirty(); std::this_thread::sleep_for(10ms); }
    check(produced == 2);
    check(std::chrono::steady_clock::now() - t0 < 1s);

    // flush_all reaches a registered saver and waits for it.
    generation = 7;
    check(cs::flush_all("exit", 2000ms));
    check(slurp(dir / "lib.bin") == "gen7");

    // stop() writes once more, then the saver leaves the registry: flush_all has nothing to wait on.
    generation = 9;
    saver.stop();
    check(slurp(dir / "lib.bin") == "gen9");
    const int after_stop = produced;
    check(cs::flush_all("exit", 100ms));
    check(produced == after_stop);
  }

  // Producers choose: Nothing writes nothing, Remove deletes the file (a faulted library).
  {
    const fs::path p = dir / "faulted.bin";
    check(cs::write_file_atomic(p.string(), "bad", 3));
    std::atomic<int> mode{0};
    std::atomic<int> saves{0};   // before the saver: its last save (in its destructor) may call back
    cs::Saver saver(10ms, 50ms);
    saver.add(p.string(), [&](cs::Buffer&) { return mode == 0 ? cs::Produce::Nothing : cs::Produce::Remove; });
    saver.on_saved([&](uint64_t, const char*) { ++saves; });
    saver.start();
    check(saver.flush("flush", 1000ms));
    check(fs::exists(p) && saves == 0);
    mode = 1;
    check(saver.flush("flush", 1000ms));
    check(!fs::exists(p) && saves == 1);
  }

  // Queued bytes (a linked GL program) are written by the saver thread.
  {
    cs::Saver saver(10ms, 50ms);
    saver.start();
    saver.enqueue((dir / "prog.bin").string(), std::vector<uint8_t>{1, 2, 3});
    check(saver.flush("flush", 1000ms));
    check(slurp(dir / "prog.bin") == std::string("\x01\x02\x03", 3));
    check(saver.saved_bytes() == 3);
  }

  // A write that fails (here: its folder is missing) is reported to the producer and tried again a
  // few times; once the folder is back, the next save writes it.
  {
    const fs::path folder = dir / "later";
    std::atomic<int> failed{0}, produced{0};
    cs::Saver saver(10ms, 50ms);
    saver.add((folder / "c.bin").string(), [&](cs::Buffer& out) { ++produced; out.assign("\x07", 1); return cs::Produce::Write; },
              [&] { ++failed; });
    saver.start();
    saver.mark_dirty();
    for (int i = 0; i < 200 && failed < 3; ++i) std::this_thread::sleep_for(5ms);
    std::this_thread::sleep_for(100ms);
    check(failed == 3);          // retried, then left alone (kMaxAttempts)
    const int tries = produced;
    std::this_thread::sleep_for(100ms);
    check(produced == tries);    // no endless retry while the disk stays unwritable
    fs::create_directories(folder);
    check(saver.flush("flush", 1000ms));
    check(slurp(folder / "c.bin") == std::string("\x07", 1));
  }

  // A producer that throws costs that save only; the saver keeps going.
  {
    std::atomic<int> calls{0};
    cs::Saver saver(10ms, 50ms);
    saver.add((dir / "t.bin").string(), [&](cs::Buffer& out) {
      if (++calls == 1) throw std::runtime_error("out of memory");
      out.assign("\x01", 1);
      return cs::Produce::Write;
    });
    saver.start();
    check(cs::flush_all_result("flush", 1000ms) == cs::FlushResult::Failed);   // distinct from timeout
    check(!fs::exists(dir / "t.bin"));
    check(saver.flush("flush", 1000ms));
    check(slurp(dir / "t.bin") == std::string("\x01", 1));
  }

  // Old temporaries a killed process left behind are removed when a saver starts (on its thread,
  // before its first save); fresh ones (another copy of the game writing right now) and other files
  // stay. A folder added for queued files is cleaned of any file's temporaries.
  {
    check(cs::detail::temporary_of("s.bin.123-0.tmp", "s.bin"));
    check(!cs::detail::temporary_of("s.binx.1-0.tmp", "s.bin"));
    check(!cs::detail::temporary_of("s.bin.tmp", "s.bin"));
    check(!cs::detail::temporary_of("s.bin.12a-0.tmp", "s.bin"));
    check(cs::detail::temporary_of("prog-ab-cd.bin.77-3.tmp", ""));
    check(!cs::detail::temporary_of("notes.tmp", ""));
    const fs::path old_tmp = dir / "s.bin.123-0.tmp", new_tmp = dir / "s.bin.456-0.tmp", other = dir / "s.binx.1-0.tmp";
    for (const auto& p : {old_tmp, new_tmp, other}) check(cs::write_file_atomic(p.string(), "x", 1));
    fs::last_write_time(old_tmp, fs::file_time_type::clock::now() - std::chrono::hours(1));
    fs::last_write_time(other, fs::file_time_type::clock::now() - std::chrono::hours(1));
    cs::Saver saver(10ms, 50ms);
    saver.add((dir / "s.bin").string(), [](cs::Buffer&) { return cs::Produce::Nothing; });
    saver.start();
    check(saver.flush("flush", 1000ms));   // the scan runs on the saver thread, before any save
    check(!fs::exists(old_tmp));
    check(fs::exists(new_tmp) && fs::exists(other));
    fs::remove(new_tmp);
    fs::remove(other);

    const fs::path progs = dir / "progs";
    fs::create_directories(progs);
    const fs::path old_prog = progs / "prog-1-2.bin.9-0.tmp", new_prog = progs / "prog-3-4.bin.9-1.tmp", keep = progs / "prog-1-2.bin";
    for (const auto& p : {old_prog, new_prog, keep}) check(cs::write_file_atomic(p.string(), "x", 1));
    fs::last_write_time(old_prog, fs::file_time_type::clock::now() - std::chrono::hours(1));
    cs::Saver folder_saver(10ms, 50ms);
    folder_saver.add_folder(progs.string());
    folder_saver.start();
    check(folder_saver.flush("flush", 1000ms));
    check(!fs::exists(old_prog) && fs::exists(new_prog) && fs::exists(keep));
    fs::remove_all(progs);
  }

  // Buffer: keeps its storage between saves and grows with room to spare.
  {
    cs::Buffer b;
    b.resize(100);
    check(b.size() == 100 && b.capacity() >= 125);
    uint8_t* const storage = b.data();
    b.clear();
    b.resize(110);   // within the room left: the same storage, no new allocation
    check(b.data() == storage && b.size() == 110);
    b.assign("abc", 3);
    b.resize(1000);  // growing keeps what is there
    check(b.size() == 1000 && std::memcmp(b.data(), "abc", 3) == 0);
  }

  // A dirty mark that arrives while a producer runs is saved by the next round, without another mark.
  {
    std::atomic<int> produced{0};
    std::atomic<bool> inside{false}, marked{false};
    cs::Saver saver(20ms, 100ms);
    saver.add((dir / "race.bin").string(), [&](cs::Buffer& out) {
      if (++produced == 1) {
        inside = true;
        while (!marked) std::this_thread::sleep_for(1ms);
      }
      out.assign("r", 1);
      return cs::Produce::Write;
    });
    saver.start();
    saver.mark_dirty();
    for (int i = 0; i < 400 && !inside; ++i) std::this_thread::sleep_for(5ms);
    check(inside);
    saver.mark_dirty();   // while the first save's producer runs
    marked = true;
    for (int i = 0; i < 400 && produced < 2; ++i) std::this_thread::sleep_for(5ms);
    check(produced == 2);
  }

  // A flush asked for while a timed save runs gets a save of its own after it, with what is new.
  {
    std::atomic<int> produced{0}, generation{1};
    std::atomic<bool> inside{false}, release{false};
    cs::Saver saver(10ms, 50ms);
    saver.add((dir / "timed.bin").string(), [&](cs::Buffer& out) {
      if (++produced == 1) {
        inside = true;
        while (!release) std::this_thread::sleep_for(1ms);
      }
      const std::string s = "gen" + std::to_string(generation.load());
      out.assign(s.data(), s.size());
      return cs::Produce::Write;
    });
    saver.start();
    saver.mark_dirty();
    for (int i = 0; i < 400 && !inside; ++i) std::this_thread::sleep_for(5ms);
    check(inside);
    generation = 2;
    const uint64_t ticket = saver.request_flush("flush");
    release = true;
    check(saver.wait_flushed(ticket, cs::Saver::Clock::now() + 2s));
    check(produced == 2);
    check(slurp(dir / "timed.bin") == "gen2");
  }

  // flush_all gives up at its deadline behind a producer that does not return, and says so; the
  // split form (an exit sending its Leave meanwhile) waits only on savers still alive.
  {
    std::atomic<bool> inside{false}, release{false};
    cs::Saver saver(10ms, 50ms);
    saver.add((dir / "blocked.bin").string(), [&](cs::Buffer& out) {
      inside = true;
      while (!release) std::this_thread::sleep_for(1ms);
      out.assign("b", 1);
      return cs::Produce::Write;
    });
    saver.start();
    const auto t0 = std::chrono::steady_clock::now();
    check(cs::flush_all_result("exit", 100ms) == cs::FlushResult::Timeout);
    const auto waited = std::chrono::steady_clock::now() - t0;
    check(waited >= 90ms && waited < 1s);
    check(inside);
    release = true;
    check(saver.flush("flush", 2000ms));
    check(slurp(dir / "blocked.bin") == "b");

    const auto tickets = cs::request_flush_all("exit");
    check(!tickets.tickets.empty());
    saver.stop();   // stops (and saves) between the request and the wait
    check(cs::wait_flush_all(tickets, cs::Saver::Clock::now() + 100ms));
  }

  // A failed write is reported by the flush that asked for it, and the next flush that writes it
  // reports success.
  {
    const fs::path folder = dir / "absent";
    std::atomic<int> failed{0};   // before the saver: its last save (in its destructor) may call back
    cs::Saver saver(10ms, 50ms);
    saver.add((folder / "f.bin").string(), [&](cs::Buffer& out) { out.assign("f", 1); return cs::Produce::Write; }, [&] { ++failed; });
    saver.start();
    check(!saver.flush("flush", 1000ms));
    check(failed >= 1);
    check(!cs::flush_all("exit", 1000ms));
    fs::create_directories(folder);
    check(saver.flush("flush", 1000ms));
    check(cs::flush_all("exit", 1000ms));
    check(slurp(folder / "f.bin") == "f");
  }

  // A producer that cannot copy now (Failed) is retried, and its flush reports it.
  {
    std::atomic<int> calls{0}, failed{0};
    cs::Saver saver(10ms, 50ms);
    saver.add((dir / "grew.bin").string(), [&](cs::Buffer& out) {
      if (++calls == 1) return cs::Produce::Failed;
      out.assign("g", 1);
      return cs::Produce::Write;
    }, [&] { ++failed; });
    saver.start();
    check(!saver.flush("flush", 1000ms));
    check(failed >= 1);
    for (int i = 0; i < 200 && calls < 2; ++i) std::this_thread::sleep_for(5ms);   // retried without a new mark
    check(calls >= 2);
    check(saver.flush("flush", 1000ms));
    check(slurp(dir / "grew.bin") == "g");
  }

  // A hold (a match) keeps timed saves back; a flush still writes, and lifting the hold saves.
  {
    std::atomic<int> produced{0};
    cs::Saver saver(10ms, 30ms);
    saver.add((dir / "held.bin").string(), [&](cs::Buffer& out) { ++produced; out.assign("h", 1); return cs::Produce::Write; });
    saver.start();
    saver.hold(true);
    saver.mark_dirty();
    std::this_thread::sleep_for(150ms);
    check(produced == 0);
    check(saver.flush("relaunch", 1000ms));
    check(produced == 1);
    saver.mark_dirty();
    std::this_thread::sleep_for(100ms);
    check(produced == 1);
    saver.hold(false);
    for (int i = 0; i < 200 && produced < 2; ++i) std::this_thread::sleep_for(5ms);
    check(produced == 2);
  }

  // The progress frame: inside the target at every size, the bar grows with progress.
  {
    namespace pf = gx::progress_frame;
    const int sizes[][2] = {{1280, 720}, {1920, 1080}, {3840, 2160}, {640, 480}, {320, 200}, {64, 32}, {1, 1}};
    for (auto& s : sizes) {
      const auto rects = pf::layout(s[0], s[1], 312, 840);
      check(!rects.empty());
      for (const auto& r : rects) check(r.x >= 0 && r.y >= 0 && r.w > 0 && r.h > 0 && r.x + r.w <= s[0] && r.y + r.h <= s[1]);
    }
    check(pf::layout(0, 720, 1, 2).empty());
    auto fill_width = [](const std::vector<pf::Rect>& rects) {
      for (const auto& r : rects) if (r.rgba == pf::kFill) return r.w;
      return 0;
    };
    check(fill_width(pf::layout(1280, 720, 0, 840)) == 0);
    const int half = fill_width(pf::layout(1280, 720, 420, 840));
    const int full = fill_width(pf::layout(1280, 720, 840, 840));
    check(half > 0 && full > half && std::abs(full - 2 * half) <= 1);
    check(fill_width(pf::layout(1280, 720, 900, 840)) == full);   // done past total stays full
    check(pf::counts(312, 840) == "312 / 840");
    // The title fits on one line at 1280 wide.
    check(pf::text_width(pf::kTitle, 1) * 2 <= 1280);
    pf::Pacer pacer(50ms, 20ms);
    check(!pacer.due());
    std::this_thread::sleep_for(60ms);
    check(pacer.due());
    check(!pacer.due());
  }

  // Last, since a freeze is for good: a freeze while a producer runs keeps the old file (no rename),
  // leaves no temporary, and the flush reports it; after it no producer runs and nothing is written.
  {
    const fs::path p = dir / "frozen.bin";
    check(cs::write_file_atomic(p.string(), "old", 3));
    std::atomic<int> calls{0};
    cs::Saver saver(10ms, 50ms);
    saver.add(p.string(), [&](cs::Buffer& out) {
      ++calls;
      cs::freeze();   // a crash on another thread, mid-save
      out.assign("new", 3);
      return cs::Produce::Write;
    });
    saver.start();
    check(!saver.flush("flush", 1000ms));
    check(calls == 1);
    check(slurp(p) == "old");
    check(temporaries(dir) == 0);
    check(!saver.flush("flush", 1000ms));
    saver.stop();
    check(calls == 1);   // neither the flush nor the last save ran the producer again
    check(!cs::write_file_atomic(p.string(), "x", 1));
    check(slurp(p) == "old");
  }

  // prewarm_variants.h: the start waits for one variant, the one this session's draws use.
  {
    namespace pv = gx::prewarm_variants;
    // Upscaler off on a PC that has one (XeSS on pc3's AMD iGPU): plain waited for, motion vectors after.
    auto p = pv::plan(false, true, 1, false);
    check(p.count == 2);
    check(p.variants[0].mvec == 0 && p.variants[0].wait);
    check(p.variants[1].mvec == pv::kMotionVectors && !p.variants[1].wait);
    // A session that saved only plain pipelines leaves nothing to wait for on the next start.
    int waited_mvec = 0;
    for (int i = 0; i < p.count; ++i) if (p.variants[i].wait && pv::motion_vectors(p.variants[i].mvec)) ++waited_mvec;
    check(waited_mvec == 0);
    // Upscaler on: the motion-vector variant is the one waited for.
    p = pv::plan(true, true, 1, false);
    check(p.count == 2 && p.variants[0].mvec == pv::kMotionVectors && p.variants[0].wait);
    check(p.variants[1].mvec == 0 && !p.variants[1].wait);
    // No upscaler on this PC: one variant only.
    p = pv::plan(false, false, 1, false);
    check(p.count == 1 && p.variants[0].mvec == 0 && p.variants[0].wait);
    // MSAA: the plain key carries the sample count, as get_pso's does (msaa_samples_ << 8).
    p = pv::plan(false, true, 4, false);
    check(p.variants[0].mvec == (4u << 8) && p.variants[1].mvec == pv::kMotionVectors);
    // The precompile pass ends after the prewarm: it waits for every variant.
    p = pv::plan(false, true, 1, true);
    check(p.count == 2 && p.variants[0].wait && p.variants[1].wait);
    // Recipes kept: plain and upscaler at any MSAA count; never G-buffer, HUD-mask or Remix keys.
    check(pv::keeps_recipe(0) && pv::keeps_recipe(1) && pv::keeps_recipe(4u << 8) && pv::keeps_recipe(8u << 8));
    check(!pv::keeps_recipe(2) && !pv::keeps_recipe(8) && !pv::keeps_recipe(16 | 1) && !pv::keeps_recipe(32 | 16 | 1));
  }

  fs::remove_all(dir);
  std::printf("pipeline_cache_store_test passed\n");
  return 0;
}
