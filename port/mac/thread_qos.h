// Scheduling requests for the Mac game's latency-critical threads.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#ifdef __APPLE__
#include <pthread.h>
#include <pthread/qos.h>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace host { void log(const char* fmt, ...); }
namespace mac_host {
inline const char* qos_name(qos_class_t qos) {
  switch (qos) {
    case QOS_CLASS_USER_INTERACTIVE: return "user-interactive";
    case QOS_CLASS_USER_INITIATED: return "user-initiated";
    case QOS_CLASS_DEFAULT: return "default";
    case QOS_CLASS_UTILITY: return "utility";
    case QOS_CLASS_BACKGROUND: return "background";
    case QOS_CLASS_UNSPECIFIED: return "unset";
    default: return "unknown";
  }
}
inline const char* current_thread_qos_name() {
  qos_class_t qos = QOS_CLASS_UNSPECIFIED;
  return pthread_get_qos_class_np(pthread_self(), &qos, nullptr) == 0 ? qos_name(qos) : "unset";
}
// Simulation and render/submit threads can report directly. Never call this from audio.
// Once per actual thread, including a refused request; never retry per frame.
inline void interactive_thread_qos_once(const char* name) {
  thread_local const bool requested = [name] {
    const int rc = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    host::log("mac: thread %s QoS request user-interactive result %d (%s), now %s",
              name, rc, rc ? std::strerror(rc) : "ok", current_thread_qos_name());
    return true;
  }();
  (void)requested;
}
// One packed publication keeps the result and queried class from different threads
// from being paired. SDL has one callback thread; audio_close drains after joining
// it, before a replacement can publish. All formatting/logging stays on the consumer.
constexpr uint64_t audio_qos_empty = UINT64_MAX;
inline std::atomic<uint64_t> audio_qos_result{audio_qos_empty};
static_assert(std::atomic<uint64_t>::is_always_lock_free, "audio QoS publication must be lock-free");
inline void audio_thread_qos_once() {
  thread_local bool requested = false;
  if (requested) return;
  requested = true;
  const int rc = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
  qos_class_t qos = QOS_CLASS_UNSPECIFIED;
  if (pthread_get_qos_class_np(pthread_self(), &qos, nullptr) != 0) qos = QOS_CLASS_UNSPECIFIED;
  const uint64_t result = (uint64_t(qos) << 32) | uint32_t(rc);
  audio_qos_result.store(result, std::memory_order_relaxed);
}
// Called by the main thread's window/embed pump, and after SDL closes the device.
inline void report_audio_thread_qos() {
  const uint64_t result = audio_qos_result.exchange(audio_qos_empty, std::memory_order_relaxed);
  if (result == audio_qos_empty) return;
  const int rc = int(uint32_t(result));
  host::log("mac: thread SDL audio callback QoS request user-interactive result %d (%s), now %s",
            rc, rc ? std::strerror(rc) : "ok", qos_name(qos_class_t(result >> 32)));
}
}  // namespace mac_host
#endif
