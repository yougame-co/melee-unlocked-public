// Windows-only observers; never used to choose an interval, flag, deadline, or wait.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "pacing_diagnostics.h"
#include "host.h"
#include "window.h"
#include "yougame_link.h"
#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

namespace gx::pacing_diag {
class WindowsRecorder {
 public:
  // Reuse the existing VRR query every 32 presents, at its original call site.
  // No additional GetFrameStatistics call and no change to the VRR detector input.
  void statistics(HRESULT hr, const DXGI_FRAME_STATISTICS& s) {
    if (SUCCEEDED(hr)) statistics_.add({s.PresentCount, s.PresentRefreshCount, s.SyncRefreshCount});
    else statistics_.unavailable(hr == DXGI_ERROR_FRAME_STATISTICS_DISJOINT);
  }
  void present(double present_seconds, bool vsync, unsigned interval, unsigned flags, double cap, bool exclusive) {
    frames_.present(present_seconds); // Existing CPU timestamp immediately before Present.
    pending_ = true; now_ = present_seconds; vsync_ = vsync; interval_ = interval;
    flags_ = flags; cap_ = cap; exclusive_ = exclusive;
  }
  // Run metadata/log work after the backend's existing GPU signals and timing measurements.
  void poll(IDXGISwapChain* chain, HWND hwnd) {
    if (!pending_) return;
    pending_ = false;
    const double present_seconds = now_, cap = cap_;
    const bool vsync = vsync_;
    const unsigned interval = interval_, flags = flags_;
    if (!started_) {
      started_ = true; minute_ = present_seconds;
      capabilities(chain);
      host::log("pacing: statistics counters are lower bounds; never-shown means no separate refresh with tearing; samples/unavailable/disjoint report DXGI coverage; max latency 0 means unknown");
    }
    // Metadata and mode events are polled at most once per second, only while presenting.
    if (!polled_ || present_seconds - second_ >= 1.0) {
      polled_ = true; second_ = present_seconds;
      metadata(chain, hwnd, exclusive_);
    }
    if (present_seconds - minute_ < 60.0) return;
    // The current cap defines late: monitor for -1, display cadence for uncapped 0.
    const double target_hz = cap > 0 ? cap : hz_;
    const auto s = frames_.summarize(target_hz > 0 ? 1000.0 / target_hz : 0);
    const auto c = statistics_.take();
    host::log("pacing: %.3f Hz display, vsync %s, interval %u, tearing %s, cap %.3f, frames %llu, frame ms p50 %.3f p95 %.3f p99 %.3f max %.3f, late(>1.5x) %llu, shown-twice %llu, never-shown %llu, composition %s, samples %zu, stats samples %llu unavailable %llu disjoint %llu",
        hz_, vsync ? "on" : "off", interval, (flags & DXGI_PRESENT_ALLOW_TEARING) ? "on" : "off", cap,
        (unsigned long long)s.frames, s.p50, s.p95, s.p99, s.maximum, (unsigned long long)s.late,
        (unsigned long long)c.shown_twice, (unsigned long long)c.never_shown, composition_, s.samples,
        (unsigned long long)c.samples, (unsigned long long)c.unavailable, (unsigned long long)c.disjoint);
    minute_ = present_seconds;
  }
 private:
  void capabilities(IDXGISwapChain* chain) {
    DXGI_SWAP_CHAIN_DESC d{}; chain->GetDesc(&d);
    Microsoft::WRL::ComPtr<IDXGIFactory5> factory;
    BOOL tearing = FALSE;
    if (SUCCEEDED(chain->GetParent(IID_PPV_ARGS(&factory))))
      factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof tearing);
    UINT latency = 0;
    Microsoft::WRL::ComPtr<IDXGISwapChain2> sc2;
    if (SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&sc2)))) sc2->GetMaximumFrameLatency(&latency);
    if (!latency) {
      Microsoft::WRL::ComPtr<IDXGIDevice1> device;
      if (SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&device)))) device->GetMaximumFrameLatency(&latency);
    }
    host::log("present: swap chain flip-model %s, tearing supported %s, waitable %s, max latency %u",
        d.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD || d.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ? "yes" : "no",
        tearing ? "yes" : "no", (d.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) ? "yes" : "no", latency);
  }
  void metadata(IDXGISwapChain* chain, HWND hwnd, bool exclusive) {
    // Observe the actual HWND: adoption may fail even when an embed launch was requested.
    const bool child = (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD) != 0;
    const int window = child ? 0 : yougame::embed_toplevel() && GetWindow(hwnd, GW_OWNER) ? 1 : 2;
    int mode = exclusive ? 3 : 2;
    if (window != 2) {
      RECT r{}; GetClientRect(hwnd, &r);
      POINT origin{}; ClientToScreen(hwnd, &origin); OffsetRect(&r, origin.x, origin.y);
      MONITORINFO monitor{sizeof monitor};
      // App fullscreen has no bridge mode bit: infer from client coverage of the output.
      // Polling may miss transitions shorter than a second; clipping/letterboxing can hide coverage.
      const bool covers = GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor) &&
          r.left <= monitor.rcMonitor.left + 2 && r.top <= monitor.rcMonitor.top + 2 &&
          r.right >= monitor.rcMonitor.right - 2 && r.bottom >= monitor.rcMonitor.bottom - 2;
      mode = covers ? 1 : 0;
    }
    if (window != window_ || mode != mode_) {
      static const char* windows[] = {"embedded", "toplevel", "standalone"};
      static const char* modes[] = {"box", "app-fullscreen", "windowed", "exclusive"};
      host::log("present: window %s, mode %s", windows[window], modes[mode]);
      window_ = window; mode_ = mode;
    }
    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    DXGI_OUTPUT_DESC desc{};
    DEVMODEW current{}; current.dmSize = sizeof current;
    double hz = 0;
    // Current mode on THIS swap chain's output, rather than the primary monitor.
    if (SUCCEEDED(chain->GetContainingOutput(&output)) && SUCCEEDED(output->GetDesc(&desc)) &&
        EnumDisplaySettingsExW(desc.DeviceName, ENUM_CURRENT_SETTINGS, &current, 0) && current.dmDisplayFrequency > 1)
      hz = current.dmDisplayFrequency;
    if (hz > 0) {
      // GetDisplayModeList supplies rational rates. EnumDisplaySettings identifies
      // the current dimensions/nominal rate; retain its integer Hz if multiple
      // fractional modes match (do not guess which 59.94/60 mode is active).
      std::array<DXGI_MODE_DESC, 256> modes{};
      UINT count = static_cast<UINT>(modes.size());
      if (SUCCEEDED(output->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, 0, &count, modes.data()))) {
        double rate = 0;
        bool ambiguous = false;
        for (UINT i = 0; i < count; ++i) {
          const auto& m = modes[i];
          if (m.Width != current.dmPelsWidth || m.Height != current.dmPelsHeight || !m.RefreshRate.Denominator) continue;
          const double candidate = double(m.RefreshRate.Numerator) / m.RefreshRate.Denominator;
          if (std::fabs(candidate - hz) >= 1.0) continue;
          if (rate > 0 && std::fabs(candidate - rate) > .001) ambiguous = true;
          rate = candidate;
        }
        if (rate > 0 && !ambiguous) hz = rate;
      } // More than 256 modes or an unavailable list: keep current nominal Hz.
    }
    if (hz != hz_) {
      host::log("display: refresh %.3f -> %.3f Hz", hz_, hz);
      hz_ = hz;
    }
    composition_ = "unknown";
    DXGI_FRAME_STATISTICS_MEDIA media{};
    Microsoft::WRL::ComPtr<IDXGISwapChainMedia> media_interface;
    if (SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&media_interface))) &&
        SUCCEEDED(media_interface->GetFrameStatisticsMedia(&media))) {
      switch (media.CompositionMode) {
        case DXGI_FRAME_PRESENTATION_MODE_COMPOSED: composition_ = "composed"; break;
        case DXGI_FRAME_PRESENTATION_MODE_OVERLAY: composition_ = "overlay"; break;
        // NONE means no composition/overlay: the independent presentation path.
        case DXGI_FRAME_PRESENTATION_MODE_NONE: composition_ = "independent"; break;
        case DXGI_FRAME_PRESENTATION_MODE_COMPOSITION_FAILURE: break;
      }
    }
  }
  FrameRing<> frames_;
  Statistics statistics_;
  bool pending_ = false, vsync_ = false, exclusive_ = false;
  double now_ = 0, cap_ = 0;
  unsigned interval_ = 0, flags_ = 0;
  bool started_ = false, polled_ = false;
  double minute_ = 0, second_ = 0, hz_ = 0;
  int window_ = -1, mode_ = -1;
  const char* composition_ = "unknown";
};
} // namespace gx::pacing_diag
