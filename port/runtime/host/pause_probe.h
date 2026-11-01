// Opt-in Mac pause diagnostics; the gap policy is GPU/OS independent for fake-clock checks.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace host::pause_probe {
constexpr uint64_t ms = 1000000;
inline bool pacing_gap(uint64_t requested, uint64_t elapsed) {
  return elapsed > requested && elapsed - requested > 250 * ms;
}
struct GapWatch {
  uint64_t last_wake;
  uint64_t sampled_progress = UINT64_MAX;
  unsigned samples = 0;
  struct Event { uint64_t gap; bool sample; };
  Event poll(uint64_t now, uint64_t progress) {
    const uint64_t gap = now >= last_wake ? now - last_wake : 0;
    last_wake = now;
    const bool sample = now > progress && now - progress > 500 * ms &&
                        progress != sampled_progress && samples < 3;
    if (sample) { sampled_progress = progress; ++samples; }
    return {gap > 250 * ms ? gap : 0, sample};
  }
};
struct MemoryCadence {
  unsigned ticks = 0;
  bool poll(bool diagnostic) {
    if (++ticks < 10 && !diagnostic) return false;
    ticks = 0;
    return true;
  }
};
} // namespace host::pause_probe

#if defined(__APPLE__) && !defined(PAUSE_PROBE_POLICY_ONLY)
#include "sim_work.h"
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/mach_vm.h>
#include <mach/task_info.h>
#include <mach/vm_statistics.h>
#include <pthread.h>
#include <libproc.h>
#include <sys/time.h>
#include <unistd.h>
#include <dlfcn.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <thread>
#if __has_feature(ptrauth_calls)
#include <ptrauth.h>
#endif

namespace host { void log(const char*, ...); }
namespace host::pause_probe {
inline bool enabled() {
  // test_env::getenv deliberately hides diagnostics in YouGame; this probe must work there.
  static const bool on = [] {
    const char* value = std::getenv("MELEE_PAUSE_PROBE");
    return value && std::strcmp(value, "1") == 0;
  }();
  return on;
}

struct Probe {
  mach_timebase_info_data_t timebase{};
  uint64_t origin = 0;
  double process_age = 0;
  thread_t main_thread = MACH_PORT_NULL;
  uintptr_t stack_lo = 0, stack_hi = 0;
  std::atomic<uint64_t> progress{0};
  std::atomic<perf::Stage> phase{perf::Stage::Step};
  std::atomic<bool> stop{false};
  std::thread worker;
  vm_statistics64_data_t previous_vm{}; // Worker-only, last successful memory sample.
  uint64_t previous_vm_tick = 0;
  bool previous_vm_valid = false;

  uint64_t ns(uint64_t ticks) const {
    return static_cast<uint64_t>(static_cast<long double>(ticks) * timebase.numer / timebase.denom);
  }
  double uptime(uint64_t ticks) const { return process_age + ns(ticks - origin) / 1e9; }
  static const char* phase_name(perf::Stage stage) {
    switch (stage) {
    case perf::Stage::Step: return "guest/host step";
    case perf::Stage::Decode: return "GX decode";
    case perf::Stage::Submit: return "GPU submit";
    case perf::Stage::Input: return "input";
    case perf::Stage::Wait: return "pacing/wait";
    default: return "unknown";
    }
  }

  void memory(uint64_t now, bool gap, bool stack) {
    task_vm_info_data_t task{};
    mach_msg_type_number_t task_count = TASK_VM_INFO_COUNT;
    const kern_return_t task_result = task_info(mach_task_self(), TASK_VM_INFO,
        reinterpret_cast<task_info_t>(&task), &task_count);
    const bool task_valid = task_result == KERN_SUCCESS && task_count >= TASK_VM_INFO_REV1_COUNT;
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t vm_count = HOST_VM_INFO64_COUNT;
    const host_t host = mach_host_self();
    const kern_return_t vm_result = host_statistics64(host, HOST_VM_INFO64,
        reinterpret_cast<host_info64_t>(&vm), &vm_count);
    mach_port_deallocate(mach_task_self(), host);
    const bool vm_valid = vm_result == KERN_SUCCESS && vm_count >= HOST_VM_INFO64_COUNT;
    const bool delta_valid = vm_valid && previous_vm_valid;
    const auto delta = [delta_valid](uint64_t current, uint64_t previous) -> unsigned long long {
      return delta_valid && current >= previous ? current - previous : 0;
    };
    // No routine logging. Deltas span sampled heartbeat ticks (normally ~100 ms),
    // not an invented 10 ms window. All APIs/logging run outside thread suspension.
    if (gap || stack) {
      log("pause: memory at uptime %.3f s heartbeat_gap=%d stack_attempt=%d task_status=%d task_valid=%d phys_footprint_bytes=%llu compressed_bytes=%llu internal_bytes=%llu",
          uptime(now), gap, stack, task_result, task_valid,
          static_cast<unsigned long long>(task_valid ? task.phys_footprint : 0),
          static_cast<unsigned long long>(task_valid ? task.compressed : 0),
          static_cast<unsigned long long>(task_valid ? task.internal : 0));
      log("pause: system memory host_status=%d host_valid=%d delta_valid=%d since_previous_memory_tick_ms=%.3f pageins_delta=%llu pageouts_delta=%llu compressions_delta=%llu decompressions_delta=%llu swapins_delta=%llu swapouts_delta=%llu faults_delta=%llu free_pages=%u compressor_pages=%u",
          vm_result, vm_valid, delta_valid, delta_valid ? ns(now - previous_vm_tick) / 1e6 : 0,
          delta(vm.pageins, previous_vm.pageins), delta(vm.pageouts, previous_vm.pageouts),
          delta(vm.compressions, previous_vm.compressions), delta(vm.decompressions, previous_vm.decompressions),
          delta(vm.swapins, previous_vm.swapins), delta(vm.swapouts, previous_vm.swapouts),
          delta(vm.faults, previous_vm.faults), vm_valid ? vm.free_count : 0,
          vm_valid ? vm.compressor_page_count : 0);
    }
    if (vm_valid) {
      previous_vm = vm;
      previous_vm_tick = now;
      previous_vm_valid = true;
    }
  }

  void sample(unsigned attempt, uint64_t now, uint64_t last_progress) {
    // No allocation, logging, symbolication or user locks while the target is suspended.
    // Read only bounded frame records from our own captured simulation-thread stack.
    uintptr_t pcs[64]{};
    unsigned count = 0;
    const perf::Stage sampled_phase = phase.load(std::memory_order_relaxed);
    kern_return_t state_result = KERN_FAILURE, read_result = KERN_SUCCESS;
    const kern_return_t suspended = thread_suspend(main_thread);
    kern_return_t resumed = KERN_SUCCESS;
    if (suspended == KERN_SUCCESS) {
      // The main thread may have progressed since the heartbeat checked it. Do not
      // mislabel a recovered frame (or a delay in the heartbeat itself) as a stuck stack.
      if (progress.load(std::memory_order_relaxed) != last_progress) {
        const kern_return_t resume_result = thread_resume(main_thread);
        log("pause: main stack sample %u/3 skipped: frame progressed before capture (resume=%d)", attempt, resume_result);
        return;
      }
      uintptr_t pc = 0, fp = 0;
#if defined(__x86_64__)
      x86_thread_state64_t state{};
      mach_msg_type_number_t words = x86_THREAD_STATE64_COUNT;
      state_result = thread_get_state(main_thread, x86_THREAD_STATE64,
                                     reinterpret_cast<thread_state_t>(&state), &words);
      pc = state.__rip; fp = state.__rbp;
#elif defined(__aarch64__)
      arm_thread_state64_t state{};
      mach_msg_type_number_t words = ARM_THREAD_STATE64_COUNT;
      state_result = thread_get_state(main_thread, ARM_THREAD_STATE64,
                                     reinterpret_cast<thread_state_t>(&state), &words);
      pc = arm_thread_state64_get_pc(state); fp = arm_thread_state64_get_fp(state);
#endif
      if (state_result == KERN_SUCCESS && pc) pcs[count++] = pc;
      while (state_result == KERN_SUCCESS && count < 64 && fp >= stack_lo &&
             stack_hi >= 2 * sizeof(uintptr_t) && fp <= stack_hi - 2 * sizeof(uintptr_t) &&
             fp % alignof(uintptr_t) == 0) {
        uintptr_t record[2]{};
        mach_vm_size_t bytes = 0;
        read_result = mach_vm_read_overwrite(mach_task_self(), fp, sizeof record,
                                             reinterpret_cast<mach_vm_address_t>(record), &bytes);
        if (read_result != KERN_SUCCESS || bytes != sizeof record) break;
        if (!record[1]) break;
#if __has_feature(ptrauth_calls)
        record[1] = reinterpret_cast<uintptr_t>(ptrauth_strip(reinterpret_cast<void*>(record[1]), ptrauth_key_return_address));
#endif
        pcs[count++] = record[1];
        if (record[0] <= fp) break;
        fp = record[0];
      }
      resumed = thread_resume(main_thread);
    }
    log("pause: main stack sample %u/3 at uptime %.3f s, no frame progress %.3f ms, phase %s; suspend=%d state=%d read=%d resume=%d frames=%u",
        attempt, uptime(now), ns(now - last_progress) / 1e6,
        phase_name(sampled_phase), suspended, state_result, read_result, resumed, count);
    for (unsigned i = 0; i < count; ++i) {
      Dl_info info{};
      if (dladdr(reinterpret_cast<void*>(pcs[i]), &info)) {
        log("pause: stack #%u pc=%p %s+0x%llx (%s image+0x%llx)", i,
            reinterpret_cast<void*>(pcs[i]), info.dli_sname ? info.dli_sname : "?",
            static_cast<unsigned long long>(info.dli_saddr ? pcs[i] - reinterpret_cast<uintptr_t>(info.dli_saddr) : 0),
            info.dli_fname ? info.dli_fname : "?",
            static_cast<unsigned long long>(pcs[i] - reinterpret_cast<uintptr_t>(info.dli_fbase)));
      } else log("pause: stack #%u pc=%p (no symbol)", i, reinterpret_cast<void*>(pcs[i]));
    }
  }

  Probe() {
    mach_timebase_info(&timebase);
    origin = mach_absolute_time();
    proc_bsdinfo info{};
    timeval wall{};
    if (proc_pidinfo(getpid(), PROC_PIDTBSDINFO, 0, &info, sizeof info) == sizeof info &&
        gettimeofday(&wall, nullptr) == 0) {
      process_age = static_cast<double>(wall.tv_sec) - info.pbi_start_tvsec +
                    (static_cast<double>(wall.tv_usec) - info.pbi_start_tvusec) / 1e6;
    } else log("pause: process start unavailable; uptime is relative to probe start");
    main_thread = mach_thread_self();
    stack_hi = reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(pthread_self()));
    stack_lo = stack_hi - pthread_get_stacksize_np(pthread_self());
    progress.store(origin, std::memory_order_relaxed);
    perf::pause_phase = &phase;
    log("pause: probe enabled, heartbeat 10 ms, gap >250 ms, main stack >500 ms (max 3), memory every 10 ticks or gap/stack attempt, simulation thread=%u", main_thread);
    worker = std::thread([this] {
      memory(mach_absolute_time(), false, false); // Establish the first counter baseline.
      GapWatch watch{ns(mach_absolute_time())};
      MemoryCadence memory_cadence;
      while (!stop.load(std::memory_order_relaxed)) {
        // A relative 10 ms deadline avoids catch-up bursts after a long scheduling gap.
        const uint64_t before = mach_absolute_time();
        mach_wait_until(before + static_cast<uint64_t>(10 * ms) * timebase.denom / timebase.numer);
        const uint64_t now = mach_absolute_time();
        const uint64_t last = progress.load(std::memory_order_relaxed);
        const auto event = watch.poll(ns(now), ns(last));
        const auto last_phase = phase.load(std::memory_order_relaxed);
        if (memory_cadence.poll(event.gap || event.sample)) memory(now, event.gap != 0, event.sample);
        if (event.sample) sample(watch.samples, now, last);
        if (event.gap)
          log("pause: heartbeat gap %.3f ms at uptime %.3f s (main thread: %s)",
              event.gap / 1e6, uptime(now), phase_name(last_phase));
        // Symbolication/logging can be slow. Do not report our own diagnostic work
        // as another scheduler gap on the next wake.
        if (event.gap || event.sample) watch.last_wake = ns(mach_absolute_time());
      }
    });
  }
  ~Probe() {
    stop.store(true, std::memory_order_relaxed);
    if (worker.joinable()) worker.join();
    mach_port_deallocate(mach_task_self(), main_thread);
  }
};
inline Probe* active = nullptr; // Set once on the simulation thread; never accessed by the worker.
inline void frame_progress() {
  if (!enabled()) return;
  static Probe probe;
  active = &probe;
  probe.progress.store(mach_absolute_time(), std::memory_order_relaxed);
}
struct PacingPhase {
  perf::Stage previous = perf::Stage::Step;
  PacingPhase() { if (active) previous = active->phase.exchange(perf::Stage::Wait, std::memory_order_relaxed); }
  ~PacingPhase() { if (active) active->phase.store(previous, std::memory_order_relaxed); }
};
inline void pacing(uint64_t before, double requested_seconds, uint32_t frame) {
  const uint64_t after = mach_absolute_time();
  const uint64_t elapsed = active->ns(after - before);
  const uint64_t requested = static_cast<uint64_t>(requested_seconds * 1e9);
  if (pacing_gap(requested, elapsed))
    log("pause: pacing requested %.3f ms elapsed %.3f ms at sim frame %u, uptime %.3f s",
        requested_seconds * 1e3, elapsed / 1e6, frame, active->uptime(after));
}
} // namespace host::pause_probe
#endif // __APPLE__
