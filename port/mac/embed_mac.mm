// A foreign SDL/OpenGL surface following the YouGame player's window in screen points.
// SPDX-License-Identifier: GPL-2.0-or-later
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <QuartzCore/CAMetalLayer.h>
#include <SDL.h>
#include "embed_mac.h"
#include "thread_qos.h"
#include "embed_mac_state.h"
#include "embed_mac_pixels.h"
#include "embed_mac_png.h"
#include "../runtime/hle/yougame_parity.h"
#include <dispatch/dispatch.h>
#include "../runtime/hle/yougame_link.h"
#include "../runtime/hle/yougame_embed_volume.h"
#include <mach/mach.h>
#include <sys/resource.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

// host::log reaches melee.log (--log-file); declared here to keep the Win32 compatibility headers out of AppKit.
namespace host { void log(const char* fmt, ...); void audio_set_page_volume(int volume); }

@interface YGMeleeEmbedPanel : NSPanel
@end
@implementation YGMeleeEmbedPanel
- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return (self.styleMask & NSWindowStyleMaskTitled) != 0; }
// The panel sits exactly over the player box, which in full screen reaches the top of the screen. AppKit keeps ordinary
// windows out of the menu bar's band and moved the panel down; track() set it back the next tick, and full screen
// flickered (d0020's real-app check, 10-26). The fallback's titled window keeps AppKit's rule.
- (NSRect)constrainFrameRect:(NSRect)frameRect toScreen:(NSScreen*)screen {
  return (self.styleMask & NSWindowStyleMaskTitled) ? [super constrainFrameRect:frameRect toScreen:screen] : frameRect;
}
@end

// Explicit clipping: NSView defaults to clipsToBounds=NO since macOS 14.
@interface YGMeleeEmbedClipView : NSView
@end
@implementation YGMeleeEmbedClipView
- (NSView*)hitTest:(NSPoint)point {
  return NSPointInRect([self convertPoint:point fromView:self.superview], self.bounds) ? [super hitTest:point] : nil;
}
@end

namespace mac_embed {
namespace {
Millis now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct Inbox {
  std::mutex mutex;
  std::deque<Command> commands;
  bool quit = false;
};
struct Surface {
  YGMeleeEmbedPanel* panel = nil;
  NSView* picture = nil; // owned by the clipping contentView; SDL wraps this view, never the container
  SDL_Window* sdl = nullptr;
  id activation = nil, occlusion = nil;
  id key_on = nil, key_off = nil, app_on = nil, app_off = nil;   // state lines (watch_state)
  id<NSObject> awake = nil;   // the latency-critical activity that keeps App Nap off (create())
  Tracker tracker;
  SettleGate settle;
  Presentation presentation;
  std::shared_ptr<Inbox> inbox = std::make_shared<Inbox>();
  bool allowed_visible = false, embedded = true, ordered = false;
  bool wants_key = false;
  bool retina = false;
  int retina_w = 0, retina_h = 0;
  double retina_scale = 0;
  int width = 0, height = 0;
  Millis state_at = 0, follow_up_at = 0;
  const char* follow_up_reason = nullptr;
  std::string state_signature;
  Millis state_logged_at = 0;
  int state_repeats = 0;   // forced lines of an unchanged state dropped within 250 ms of the last one
  int role_resets = 0;   // keep_on_screen_role: setpriority calls
  Millis role_asked_at = 0;   // at most one request/transition line per second
  bool role_refused = false;   // setpriority said no (EPERM/ENOTSUP/EINVAL): stop asking
  bool role_promoted = false;   // only relinquish a focal role we successfully requested
  bool role_release_pending = false;   // latched until the shared request limit permits release
  Millis role_watch_until = 0;   // after any new panel frame (full-screen switches included): check every frame
#ifdef YOUGAME_AGENT_TEST_HOOK
  // agent-test.json's Mac player-box keys (yougame_agent_test.h), read once in create().
  Millis created_at = 0;
  int panel_key_after = 0, activate_after = 0;
  bool panel_key_done = false, activate_done = false;
  std::string mode;   // macPanel: "neverKey", "fsAux" or "refreshActivity"
#endif
};
struct Screenshots {
  shot::Slot slot;
  std::string data; // fixed before installing the reader callback
  bool stdio = false;
};
// A copied reader callback or cancelled encoder may outlive destroy's 500 ms
// drain. Keep this small service until process exit, with acceptance stopped;
// no static destructor can race it. Startup initialization also avoids a local
// static guard check on the idle per-present requested-flag load.
Screenshots* const screenshot_state = new Screenshots;
std::once_flag screenshot_launch;
dispatch_queue_t screenshot_worker() {
  static dispatch_queue_t queue = dispatch_queue_create("org.yougame.melee.screenshot", DISPATCH_QUEUE_SERIAL);
  return queue;
}
dispatch_group_t screenshot_group() {
  static dispatch_group_t group = dispatch_group_create();
  return group;
}
void screenshot_emit(const std::string& reply) {
  static dispatch_queue_t replies = dispatch_queue_create("org.yougame.melee.screenshot-replies", DISPATCH_QUEUE_SERIAL);
  // Never hold the lifecycle mutex across the bridge's potentially blocking writer.
  const std::string line = reply; // Blocks must own a value, never the caller's reference.
  dispatch_group_async(screenshot_group(), replies, ^{ yougame::parity::signals.emit(line.c_str()); });
}
bool screenshot_command(const std::string& line) {
  if (line.rfind("screenshot", 0) != 0) return false;
  const auto id = shot::parse(line);
  if (!id || !screenshot_state->stdio) return true; // malformed IDs have no reply identity
  if (auto request = screenshot_state->slot.begin(*id, now_ms(), screenshot_emit)) {
    const auto ticket = request->ticket;
    // Separate from the serial encoder: this fires even without presents, or during encoding.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, shot::deadline_ms * NSEC_PER_MSEC),
                   dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
      screenshot_state->slot.expire(ticket, now_ms(), screenshot_emit);
    });
  }
  return true;
}
std::unique_ptr<Surface> surface;
std::optional<uint32_t> configured_parent;
std::atomic<SDL_Window*> frame_window{nullptr};
std::atomic<bool> retina_option{false};
bool retina_launch_enabled = false; // Set in create(), before render/UI consumers start; fixed for the launch.
std::mutex pixels_mutex;
SDL_Window* pixels_window = nullptr;
PixelSize pixels_size;
double pixels_scale = 1;
PixelSize picture_points, drawable_pixels;
Rect input_picture, input_visible;
bool input_shown = false;

// Only the main thread touches the panel. Render/UI consumers read a coherent pixel snapshot.
void update_retina(Surface& s) {
  NSCAssert(NSThread.isMainThread, @"Mac embed drawable sizes must update on the main thread");
  NSView* view = s.picture;
  const BOOL best = s.embedded && s.retina ? YES : NO;
  if (view.wantsBestResolutionOpenGLSurface != best) view.wantsBestResolutionOpenGLSurface = best;
  const double scale = s.panel.backingScaleFactor > 0 ? s.panel.backingScaleFactor : 1;
  const auto size = backing_size(view.bounds.size.width, view.bounds.size.height, scale);
  const double drawable_scale = s.embedded && s.retina ? scale : 1;
  const auto drawable = backing_size(view.bounds.size.width, view.bounds.size.height, drawable_scale);
  // SDL refreshes Metal only on window-size events. A view-only resize need not resize the clip panel.
  // Find the attached view rather than retaining SDL's view past SDL_Metal_DestroyView.
  for (NSView* child in view.subviews) {
    if (![child.layer isKindOfClass:CAMetalLayer.class]) continue;
    if (!NSEqualRects(child.frame, view.bounds)) child.frame = view.bounds;
    CAMetalLayer* layer = static_cast<CAMetalLayer*>(child.layer);
    if (layer.contentsScale != drawable_scale) layer.contentsScale = drawable_scale;
    const CGSize pixels = CGSizeMake(drawable.w, drawable.h);
    if (!CGSizeEqualToSize(layer.drawableSize, pixels)) layer.drawableSize = pixels;
  }
  {
    std::lock_guard<std::mutex> lock(pixels_mutex);
    pixels_window = s.embedded ? s.sdl : nullptr;
    pixels_size = size;
    pixels_scale = scale;
    picture_points = backing_size(view.bounds.size.width, view.bounds.size.height, 1);
    drawable_pixels = s.retina ? size : picture_points;
    const NSRect f = view.frame, clip = s.panel.contentView.bounds;
    input_picture = {f.origin.x, clip.size.height - f.origin.y - f.size.height, f.size.width, f.size.height};
    input_visible = {0, 0, clip.size.width, clip.size.height};
    input_shown = s.ordered && s.panel.visible;
  }
  if (s.retina && s.embedded && (size.w != s.retina_w || size.h != s.retina_h || scale != s.retina_scale)) {
    s.retina_w = size.w; s.retina_h = size.h; s.retina_scale = scale;
    host::log("embed: retina surface %dx%d px at scale %.2f", size.w, size.h, scale);
  }
}

// Only IDs from these lists: never request window names, images or Screen Recording access.
bool contains(CFArrayRef list, CGWindowID id) {
  if (!list) return false;
  for (CFIndex i = 0; i < CFArrayGetCount(list); ++i)
    if (static_cast<CGWindowID>(reinterpret_cast<uintptr_t>(CFArrayGetValueAtIndex(list, i))) == id)
      return true;
  return false;
}
int number(CFDictionaryRef info, CFStringRef key) {
  int value = 0;
  auto n = static_cast<CFNumberRef>(CFDictionaryGetValue(info, key));
  if (n && CFGetTypeID(n) == CFNumberGetTypeID()) CFNumberGetValue(n, kCFNumberIntType, &value);
  return value;
}
Parent query_parent(CGWindowID id) {
  Parent parent;
  // OnScreenOnly excludes minimized windows and windows on other Spaces. Access only the
  // matching window's number, bounds, layer and owner PID; never kCGWindowName/owner names.
  if (CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID)) {
    for (CFIndex i = 0; i < CFArrayGetCount(list); ++i) {
      auto info = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(list, i));
      if (static_cast<CGWindowID>(number(info, kCGWindowNumber)) != id) continue;
      parent.exists = true;
      parent.owner = number(info, kCGWindowOwnerPID);
      CGRect bounds{};
      auto rect = static_cast<CFDictionaryRef>(CFDictionaryGetValue(info, kCGWindowBounds));
      if (rect && CGRectMakeWithDictionaryRepresentation(rect, &bounds)) {
        parent.bounds = {bounds.origin.x, bounds.origin.y, bounds.size.width, bounds.size.height};
        parent.on_screen = number(info, kCGWindowLayer) == 0 && bounds.size.width > 0 && bounds.size.height > 0;
      }
      break;
    }
    CFRelease(list);
  }
  if (!parent.exists) {
    // On recent macOS a minimized parent can disappear from IncludingWindow too. An ID-only
    // All list distinguishes hidden from closed, without pulling metadata for other windows.
    if (CFArrayRef all = CGWindowListCreate(kCGWindowListOptionAll, kCGNullWindowID)) {
      parent.exists = contains(all, id);
      CFRelease(all);
    }
  }
  return parent;
}
void hide(Surface& s) {
  if (s.ordered || s.panel.visible) [s.panel orderOut:nil];
  s.ordered = false;
}
void fallback(Surface& s) {
  hide(s);
  s.embedded = false;
  update_retina(s);
  s.panel.styleMask = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                      NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
  s.panel.collectionBehavior = NSWindowCollectionBehaviorDefault;
  s.panel.hidesOnDeactivate = NO;
  s.panel.hasShadow = YES;
  // Restore ordinary resizing when the parent disappears; the SDL view remains the same object.
  s.panel.contentView.autoresizesSubviews = YES;
  s.picture.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
  [s.panel setContentSize:NSMakeSize(s.width, s.height)];
  s.picture.frame = s.panel.contentView.bounds;
  [s.panel center];
  [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
  if (s.allowed_visible) {
    [s.panel makeKeyAndOrderFront:nil];
    [s.panel makeMainWindow];
  }
  host::log("embed: parent gone for 2 seconds; using a regular window");
}
// Keeps App Nap and timer coalescing off while the panel exists (see create()).
id<NSObject> begin_playing() {
  return [[NSProcessInfo.processInfo
      beginActivityWithOptions:NSActivityUserInitiatedAllowingIdleSystemSleep | NSActivityIdleDisplaySleepDisabled |
                               NSActivityLatencyCritical
      reason:@"Melee Unlocked is playing in the YouGame app"] retain];
}
// How macOS is scheduling this process, one line whenever it changes. A scheduling trace showed: once the panel
// had the keyboard focus, every full-screen stretch put all 16 of the game's threads at priority 4 within a second, and
// leaving full screen put them back at 31-54; the same walks without the focus stayed fast. Pids and relations only:
// never another app's name.
const char* role_name(int role) {
  switch (role) {
    case TASK_RENICED: return "reniced";
    case TASK_UNSPECIFIED: return "unspecified";
    case TASK_FOREGROUND_APPLICATION: return "foreground";
    case TASK_BACKGROUND_APPLICATION: return "background";
    case TASK_CONTROL_APPLICATION: return "control";
    case TASK_GRAPHICS_SERVER: return "graphics server";
    case TASK_THROTTLE_APPLICATION: return "throttle";
    case TASK_NONUI_APPLICATION: return "non-UI";
    case TASK_DEFAULT_APPLICATION: return "default";
    case TASK_DARWINBG_APPLICATION: return "darwin background";
    default: return "unknown";
  }
}
struct StateSample {
  bool active = false, key = false, visible = false, held = false;
  const char* front = "other";
  const char* main_qos = "unset";
  int role = TASK_UNSPECIFIED, threads = 0, min_priority = 0, max_priority = 0;
};
StateSample sample_ui(const Surface& s) {
  StateSample out;
  out.active = NSApp.active;
  out.key = s.panel.keyWindow;
  out.visible = (s.panel.occlusionState & NSWindowOcclusionStateVisible) != 0;
  out.held = s.awake != nil;
  const pid_t front = NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier;
  out.front = front == getpid() ? "game" : front == s.tracker.parent().owner ? "app" : "other";
  return out;
}
StateSample sample(const Surface& s) {
  StateSample out = sample_ui(s);
  // sample()/pump() and AppKit notification observers run on the simulation/main thread.
  out.main_qos = mac_host::current_thread_qos_name();
  task_category_policy_data_t policy{};
  mach_msg_type_number_t count = TASK_CATEGORY_POLICY_COUNT;
  boolean_t defaults = FALSE;
  if (task_policy_get(mach_task_self(), TASK_CATEGORY_POLICY, reinterpret_cast<task_policy_t>(&policy), &count,
                      &defaults) == KERN_SUCCESS)
    out.role = policy.role;
  thread_act_array_t threads = nullptr;
  mach_msg_type_number_t n = 0;
  if (task_threads(mach_task_self(), &threads, &n) == KERN_SUCCESS) {
    for (mach_msg_type_number_t i = 0; i < n; ++i) {
      thread_extended_info_data_t info{};
      mach_msg_type_number_t size = THREAD_EXTENDED_INFO_COUNT;
      if (thread_info(threads[i], THREAD_EXTENDED_INFO, reinterpret_cast<thread_info_t>(&info), &size) == KERN_SUCCESS) {
        out.min_priority = out.threads ? std::min(out.min_priority, info.pth_curpri) : info.pth_curpri;
        out.max_priority = out.threads ? std::max(out.max_priority, info.pth_curpri) : info.pth_curpri;
        ++out.threads;
      }
      mach_port_deallocate(mach_task_self(), threads[i]);
    }
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads), n * sizeof(thread_act_t));
  }
  return out;
}
// Keep the existing private setpriority role path, including its refusal stop. QoS alone
// cannot lift process-wide scheduling limits (Apple sys/qos.h). UI focal is only requested
// for an on-screen panel in use. Release our promotion when either gate closes;
// other inactive on-screen panels retain the existing Darwin BG recovery path.
#ifndef PRIO_DARWIN_ROLE
#define PRIO_DARWIN_ROLE 6
#endif
#ifndef PRIO_DARWIN_ROLE_UI_FOCAL
#define PRIO_DARWIN_ROLE_UI_FOCAL 1   // TASK_FOREGROUND_APPLICATION (a different enum from PRIO_DARWIN_ROLE)
#endif
#ifndef PRIO_DARWIN_ROLE_UI_NON_FOCAL
#define PRIO_DARWIN_ROLE_UI_NON_FOCAL 4   // TASK_BACKGROUND_APPLICATION
#endif
int current_role(int fallback) {
  task_category_policy_data_t policy{};
  mach_msg_type_number_t count = TASK_CATEGORY_POLICY_COUNT;
  boolean_t defaults = FALSE;
  return task_policy_get(mach_task_self(), TASK_CATEGORY_POLICY, reinterpret_cast<task_policy_t>(&policy), &count,
                         &defaults) == KERN_SUCCESS ? policy.role : fallback;
}
// isVisible changes at once on orderOut:, occlusionState a frame later: both must say on screen.
bool panel_on_screen(const Surface& s) {
  return s.ordered && s.panel.visible && !s.panel.miniaturized &&
      (s.panel.occlusionState & NSWindowOcclusionStateVisible) != 0;
}
bool panel_in_use(const StateSample& v) {
  return (v.active && v.key) || (v.visible && std::strcmp(v.front, "app") == 0);
}
void keep_on_screen_role(Surface& s, const StateSample& v) {
  if (s.role_refused) return;
  const bool on_screen = panel_on_screen(s);
  const bool in_use = panel_in_use(v);
  if (s.role_promoted && (!on_screen || !in_use)) s.role_release_pending = true;
  const bool releasing = s.role_promoted && s.role_release_pending;
  const int desired = in_use ? TASK_FOREGROUND_APPLICATION : TASK_BACKGROUND_APPLICATION;
  if (!releasing && (!on_screen || (in_use ? v.role == desired : v.role != TASK_DARWINBG_APPLICATION))) return;
  const auto now = now_ms();
  // Bound requests too, so every attempted transition gets its own before/after line.
  // A pending release survives this return, even if focus returns before the deadline.
  // watch_state() keeps checking hidden panels every 250 ms, including after the fast watcher ends.
  if (s.role_resets && now - s.role_asked_at < 1000) return;
  s.role_asked_at = now;
  const int requested = !releasing && in_use ? PRIO_DARWIN_ROLE_UI_FOCAL : PRIO_DARWIN_ROLE_UI_NON_FOCAL;
  const int rc = setpriority(PRIO_DARWIN_ROLE, 0, requested);
  const int err = rc ? errno : 0;
  if (rc == 0) {
    s.role_promoted = requested == PRIO_DARWIN_ROLE_UI_FOCAL;
    s.role_release_pending = false;
  }
  if (err == EPERM || err == ENOTSUP || err == EINVAL) s.role_refused = true;
  ++s.role_resets;
  host::log("embed: on-screen role %s -> %s; asked for role %d (%s, %s; call %d)%s",
            role_name(v.role), role_name(current_role(TASK_UNSPECIFIED)), requested,
            releasing ? (rc ? "release failed" : "released") : (in_use ? "in use" : "not in use"),
            rc ? std::strerror(err) : "ok", s.role_resets,
            s.role_refused ? "; refused, not asking again" : "");
}
// A sample every 250 ms at most (pump() runs every frame); a line when the signature changes, when asked with a
// reason, or at the follow-up a new box asked for (the role changed within a second of each full-screen switch).
// An unchanged state asked for again within 250 ms of its last line (a page sending focus every frame) is counted
// instead and the count goes on the next line. t= is wall-clock ms since 1970, to line up with outside samples.
void watch_state(Surface& s, const char* reason = nullptr) {
  const auto now = now_ms();
  const bool follow_up = s.follow_up_at && now >= s.follow_up_at;
  if (!reason && !follow_up && now - s.state_at < 250) return;
  s.state_at = now;
  const StateSample v = sample(s);   // the line shows what macOS chose; the next sample shows the reset
  keep_on_screen_role(s, v);
  char signature[128];
  std::snprintf(signature, sizeof signature, "%d%d%d %s %d %d %s", v.active, v.key, v.visible, v.front, v.role,
                v.max_priority <= 4, v.main_qos);
  if (!reason && follow_up) reason = s.follow_up_reason;
  if (follow_up) s.follow_up_at = 0;
  const bool same = s.state_signature == signature;
  if (!reason && same) return;
  if (same && now - s.state_logged_at < 250) { ++s.state_repeats; return; }
  s.state_signature = signature;
  char repeated[64] = "";
  if (s.state_repeats)
    std::snprintf(repeated, sizeof repeated, " (the line before repeated %d more times, unlogged)", s.state_repeats);
  s.state_repeats = 0;
  s.state_logged_at = now;
  const long long wall = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  host::log("embed: state t=%lld (%s): app %s, panel %s, %s, front %s, role %s, %d threads at priority %d-%d, "
            "activity %s%s, main QoS %s", wall, reason ? reason : "changed", v.active ? "active" : "inactive",
            v.key ? "key" : "not key", v.visible ? "visible" : "occluded", v.front, role_name(v.role), v.threads,
            v.min_priority, v.max_priority, v.held ? "held" : "none", repeated, v.main_qos);
}
void follow_up(Surface& s, const char* reason, Millis delay) {
  s.follow_up_at = now_ms() + delay;
  s.follow_up_reason = reason;
}
bool never_key(const Surface& s) {
#ifdef YOUGAME_AGENT_TEST_HOOK
  return s.mode == "neverKey";
#else
  (void)s;
  return false;
#endif
}
void focus_changed(Surface& s, const char* reason) {
#ifdef YOUGAME_AGENT_TEST_HOOK
  if (s.mode == "refreshActivity") {
    if (s.awake) {
      [NSProcessInfo.processInfo endActivity:s.awake];
      [s.awake release];
    }
    s.awake = begin_playing();
    host::log("embed: agent test: refreshActivity began the activity again (%s)", reason);
  }
#endif
  watch_state(s, reason);
}
#ifdef YOUGAME_AGENT_TEST_HOOK
// panelKeyAfter / activateAfter, once each, counted from create() and only once the panel is on screen.
void agent_actions(Surface& s) {
  if (!s.ordered || !s.panel.visible) return;
  const auto since = now_ms() - s.created_at;
  if (s.panel_key_after && !s.panel_key_done && since >= s.panel_key_after * 1000LL) {
    s.panel_key_done = true;
    [s.panel makeKeyWindow];
    const char* how = "makeKeyWindow";
    if (!s.panel.keyWindow) {
      // A click in the panel's bottom-left corner, the way a player's click gives a non-activating panel the key
      // without activating this process. The game sees a mouse click there too.
      const NSTimeInterval t = NSProcessInfo.processInfo.systemUptime;
      for (NSEventType type : {NSEventTypeLeftMouseDown, NSEventTypeLeftMouseUp})
        [NSApp sendEvent:[NSEvent mouseEventWithType:type location:NSMakePoint(1, 1) modifierFlags:0 timestamp:t
                                        windowNumber:s.panel.windowNumber context:nil eventNumber:0 clickCount:1
                                            pressure:type == NSEventTypeLeftMouseDown ? 1 : 0]];
      how = "makeKeyWindow, then a click";
    }
    host::log("embed: agent test: panelKeyAfter %d s (%s): panel %s", s.panel_key_after, how,
              s.panel.keyWindow ? "key" : "still not key");
    follow_up(s, "panelKeyAfter +1 s", 1000);
  }
  if (s.activate_after && !s.activate_done && since >= s.activate_after * 1000LL) {
    s.activate_done = true;
    [NSApp activateIgnoringOtherApps:YES];
    host::log("embed: agent test: activateAfter %d s: asked to become the active app", s.activate_after);
    follow_up(s, "activateAfter +1 s", 1000);
  }
}
#endif
void track(Surface& s) {
  if (!s.embedded) return;
  s.tracker.tick(now_ms(), [&] { return query_parent(*configured_parent); });
  // WindowServer may block: time inside the query is not observed stable time.
  const auto now = now_ms();
  if (s.tracker.fallback()) { fallback(s); return; }
  const Parent& parent = s.tracker.parent();
  bool show = s.allowed_visible && s.presentation.visible(parent);
  s.settle.observe(now, show, parent.bounds);
  const auto screens = NSScreen.screens;
  const double primary_height = screens.count ? screens[0].frame.size.height : 0;
  // The box is in the parent's backing pixels: convert with the app's scale when it sent one (0.13.1076+), else with
  // the scale of the display holding most of the parent (older apps). Never both: dividing twice halves the panel.
  double scale = s.presentation.app_scale;
  const char* source = "from app";
  if (!(scale > 0)) {
    source = "from display overlap";
    const Rect parent_cocoa{parent.bounds.x, primary_height - parent.bounds.y - parent.bounds.h, parent.bounds.w, parent.bounds.h};
    double most = 0;
    for (NSScreen* screen in screens) {
      const NSRect f = screen.frame;
      const double area = overlap_area(parent_cocoa, {f.origin.x, f.origin.y, f.size.width, f.size.height});
      if (area > most) { most = area; scale = screen.backingScaleFactor; }
    }
    if (!(scale > 0)) scale = NSScreen.mainScreen ? NSScreen.mainScreen.backingScaleFactor : 1;
  }
  static double logged_scale = 0;
  static const char* logged_source = nullptr;
  if (scale != logged_scale || source != logged_source) {
    logged_scale = scale; logged_source = source;
    host::log("embed: scale %.2f %s", scale, source);
  }
  const Rect picture_box = s.presentation.picture();
  const Rect visible_box = s.presentation.visible_box();
  const Rect frame = screen_rect(parent.bounds, points_from_backing(visible_box, scale), primary_height);
  const Rect picture_frame = screen_rect(parent.bounds, points_from_backing(picture_box, scale), primary_height);
  static Rect logged_picture{}, logged_visible{};
  if (!equal(logged_picture, picture_box) || !equal(logged_visible, visible_box)) {
    logged_picture = picture_box; logged_visible = visible_box;
    host::log("embed: view %.0fx%.0f at %.0f,%.0f, visible %.0fx%.0f at %.0f,%.0f",
              picture_box.w, picture_box.h, picture_box.x, picture_box.y,
              visible_box.w, visible_box.h, visible_box.x, visible_box.y);
  }
  // Size is rounded from the picture alone. NEVER derive it from rect or the container's bounds.
  // Set it even while hidden (including view-before-rect), so the first drawable has the right size.
  const auto place_picture = [&] {
    if (picture_box.w < 32 || picture_box.h < 32) return;
    const NSRect panel = s.panel.frame;
    const NSRect inner = NSMakeRect(std::round(picture_frame.x) - panel.origin.x,
                                   std::round(picture_frame.y) - panel.origin.y,
                                   std::round(picture_frame.w), std::round(picture_frame.h));
    if (!NSEqualRects(s.picture.frame, inner)) s.picture.frame = inner;
  };
  place_picture();
  bool intersects_screen = false;
  for (NSScreen* screen in screens) {
    const NSRect bounds = screen.frame;
    if (intersects(frame, {bounds.origin.x, bounds.origin.y, bounds.size.width, bounds.size.height}))
      intersects_screen = true;
  }
  show = show && intersects_screen;
  if (show && !s.ordered && !s.settle.ready(now)) show = false;
  if (!show) { hide(s); return; }
  // AppKit keeps window frames on whole points: a box on an odd backing pixel (x 70.5 pt during a full-screen
  // animation, d0025's gate 2 on r79) was placed 0.5 pt off and counted as refused. Ask for the whole points it keeps.
  const NSRect wanted = NSMakeRect(std::round(frame.x), std::round(frame.y), std::round(frame.w), std::round(frame.h));
  // A frame AppKit would not keep is not set again every tick (that flickered): up to three more tries, a second apart,
  // in case the refusal was passing (a Space switch, a full-screen animation), then it waits for the next new frame.
  static NSRect refused = NSZeroRect;
  static int refused_tries = 0;
  static Millis refused_at = 0;
  const bool retry = NSEqualRects(wanted, refused) && refused_tries < 4 && now - refused_at >= 1000;
  if (!NSEqualRects(s.panel.frame, wanted) && (!NSEqualRects(wanted, refused) || retry)) {
    [s.panel setFrame:wanted display:YES animate:NO];
    place_picture();
    const NSRect got = s.panel.frame;
    if (NSEqualRects(got, wanted)) {
      refused = NSZeroRect; refused_tries = 0;
    } else {
      if (!NSEqualRects(wanted, refused)) { refused = wanted; refused_tries = 0; }
      ++refused_tries; refused_at = now;
    }
    // Once per new frame, with where AppKit put it if not there.
    static Rect logged{};
    if (!equal(logged, frame)) {
      logged = frame;
      const Rect& box = s.presentation.box;
      const Rect& pb = parent.bounds;
      char elsewhere[96] = "";
      if (!NSEqualRects(got, wanted))
        std::snprintf(elsewhere, sizeof elsewhere, " (AppKit placed it at %.1f,%.1f %.1fx%.1f)", got.origin.x, got.origin.y,
                      got.size.width, got.size.height);
      host::log("embed: panel %.1f,%.1f %.1fx%.1f pt from box %.0f,%.0f %.0fx%.0f px at %.2fx in window %.0f,%.0f %.0fx%.0f "
                "(top-left) of a %.0f-pt-high main screen%s", wanted.origin.x, wanted.origin.y, wanted.size.width,
                wanted.size.height, box.x, box.y, box.w, box.h, scale, pb.x, pb.y, pb.w, pb.h, primary_height, elsewhere);
      // macOS flips the role 0.17-0.72 s after a full-screen switch (perf-lead, 10-28): watch it every frame for 2 s
      // after any new panel frame (resizes and drags too, harmless) so a demotion lasts a frame, not up to the 250 ms
      // state sample.
      s.role_watch_until = now_ms() + 2000;
      watch_state(s, "new box");
      follow_up(s, "new box +1 s", 1000);
    }
  }
  bool below = !s.ordered || !s.panel.visible;
  // Consume the ordering timer even on the initial order-in, keeping all queries bounded.
  if (s.tracker.ordering_due(now) && !below) {
    if (CFArrayRef above = CGWindowListCreate(kCGWindowListOptionOnScreenAboveWindow,
                                             static_cast<CGWindowID>(s.panel.windowNumber))) {
      below = contains(above, *configured_parent);
      CFRelease(above);
    }
  }
  if (below) {
    // Relative ordering preserves other apps and the parent's menus above the game.
    [s.panel orderWindow:NSWindowAbove relativeTo:*configured_parent];
    s.ordered = true;
  }
  if (s.wants_key && s.presentation.focused) {
    s.wants_key = false;
    if (!never_key(s)) {
      [s.panel makeKeyWindow]; // NSNonactivatingPanel: keyboard without activating this process
      watch_state(s, "the page's focus asked for the key");
    }
  }
}
} // namespace

bool configure(bool embedded) {
  configured_parent.reset();
  std::call_once(screenshot_launch, [] {
    screenshot_state->stdio = yougame::stdio_bridge();
    if (screenshot_state->stdio) {
      const char* data = std::getenv("YOUGAME_DATA");
      screenshot_state->data = data ? data : "";
    }
  });
  if (!embedded) {
    if (screenshot_state->stdio) yougame::set_box_handler([](const std::string& line) { screenshot_command(line); });
    return false;
  }
  const char* parent = std::getenv("YOUGAME_EMBED_PARENT");
  if (parent) configured_parent = parent_id(parent);
  unsetenv("YOUGAME_EMBED_PARENT");
  if (!configured_parent) {
    if (screenshot_state->stdio) yougame::set_box_handler([](const std::string& line) { screenshot_command(line); });
    std::fprintf(stderr, "embed: missing/invalid YOUGAME_EMBED_PARENT; using a regular SDL window\n");
    return false;
  }
  // SDL's video init must not turn this background game into the frontmost app.
  SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1");
  return true;
}
SDL_Window* create(int w, int h, const char* title, bool allowed_visible) {
  @autoreleasepool {
    NSCAssert(NSThread.isMainThread, @"Mac embed must be created on the main thread");
    if (!configured_parent) return nullptr;
    surface = std::make_unique<Surface>();
    auto& s = *surface;
    s.width = w; s.height = h; s.allowed_visible = allowed_visible;
#ifdef YOUGAME_AGENT_TEST_HOOK
    // Hook builds only: written by main.cpp from agent-test.json, which only an agent's own test copy writes.
    const auto seconds = [](const char* name) {
      const char* text = std::getenv(name);
      const int value = text ? std::atoi(text) : 0;
      unsetenv(name);
      return value >= 1 && value <= 600 ? value : 0;
    };
    s.created_at = now_ms();
    s.panel_key_after = seconds("MELEE_TEST_PANEL_KEY_AFTER");
    s.activate_after = seconds("MELEE_TEST_ACTIVATE_AFTER");
    if (const char* mode = std::getenv("MELEE_TEST_MAC_PANEL")) {
      if (!std::strcmp(mode, "neverKey") || !std::strcmp(mode, "fsAux") || !std::strcmp(mode, "refreshActivity"))
        s.mode = mode;
      unsetenv("MELEE_TEST_MAC_PANEL");
    }
    if (s.panel_key_after || s.activate_after || !s.mode.empty())
      host::log("embed: agent test: panelKeyAfter %d s, activateAfter %d s, macPanel %s", s.panel_key_after,
                s.activate_after, s.mode.empty() ? "default" : s.mode.c_str());
#endif
    s.panel = [[YGMeleeEmbedPanel alloc] initWithContentRect:NSMakeRect(0, 0, w, h)
        styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
        backing:NSBackingStoreBuffered defer:NO];
    if (!s.panel) { surface.reset(); return nullptr; }
    s.panel.restorable = NO;
    s.panel.restorationClass = Nil;
    s.panel.releasedWhenClosed = NO;
    s.panel.hidesOnDeactivate = NO;
    s.panel.floatingPanel = NO;
    s.panel.becomesKeyOnlyIfNeeded = NO;
    s.panel.opaque = YES;
    s.panel.backgroundColor = NSColor.blackColor;
    s.panel.hasShadow = NO;
    s.panel.animationBehavior = NSWindowAnimationBehaviorNone;
    s.panel.level = NSNormalWindowLevel;
    s.panel.collectionBehavior = NSWindowCollectionBehaviorMoveToActiveSpace |
        NSWindowCollectionBehaviorIgnoresCycle | NSWindowCollectionBehaviorTransient;
#ifdef YOUGAME_AGENT_TEST_HOOK
    if (s.mode == "fsAux") s.panel.collectionBehavior |= NSWindowCollectionBehaviorFullScreenAuxiliary;
#endif
    s.panel.title = [NSString stringWithUTF8String:title];
    NSView* clip = [[YGMeleeEmbedClipView alloc] initWithFrame:NSMakeRect(0, 0, w, h)];
    clip.clipsToBounds = YES;
    clip.autoresizesSubviews = NO;
    s.panel.contentView = clip;
    [clip release];
    s.picture = [[NSView alloc] initWithFrame:clip.bounds];
    s.picture.autoresizingMask = NSViewNotSizable;
    [clip addSubview:s.picture];
    [s.picture release];
    SDL_SetHint(SDL_HINT_VIDEO_FOREIGN_WINDOW_OPENGL, "1");
    // SDL 2.30.9 Cocoa_CreateWindowFrom accepts NSView and stores it as sdlContentView.
    // GL and GetWindowSizeInPixels use that view's bounds, independently of NSWindow resize events.
    s.sdl = SDL_CreateWindowFrom(s.picture);
    if (!s.sdl) { destroy(); return nullptr; }
    // SDL 2.30 leaves foreign windows at 1x. OFF preserves that path. ON must override every pixel consumer too;
    // changing only this view caused d0017/d0018's half-size lower-left/black-fullscreen failures.
    const char* retina_env = std::getenv("MELEE_MAC_EMBED_RETINA");
    s.retina = retina_enabled(retina_option.load(), retina_env);
    retina_launch_enabled = s.retina;
    update_retina(s);
    std::weak_ptr<Inbox> inbox = s.inbox;
    yougame::set_box_handler([inbox](const std::string& line) {
      if (screenshot_command(line)) return;
      if (line.rfind("volume ", 0) == 0) {
        // The page's volume control, a master over the player's own volume as on Windows and Linux (tester report
        // 1710ab56). audio_set_page_volume is an atomic store, so it needs no trip through the main thread's inbox. One
        // log line per change, so a page that sends volume can be told from a game that ignores it.
        static std::atomic<int> last{-1};
        const int volume = yg_embed_volume(line.c_str());
        if (volume < 0) host::log("embed: ignored a volume command");
        else {
          if (last.exchange(volume) != volume) host::log("embed: got volume %d", volume);
          host::audio_set_page_volume(volume);
        }
        return;
      }
      const auto command = parse_command(line);
      if (!command) return;
      // One log line per command from the app (a rect only when it changes), so a page/host disagreement about the
      // player box can be told apart.
      static std::string last_rect, last_view;
      if (command->kind == Kind::Rect || command->kind == Kind::View) {
        auto& last = command->kind == Kind::View ? last_view : last_rect;
        if (line != last) { last = line; host::log("embed: got %s", line.c_str()); }
      } else {
        host::log("embed: got %s", line.c_str());
      }
      if (auto queue = inbox.lock()) {
        std::lock_guard<std::mutex> lock(queue->mutex);
        if (command->kind == Kind::Quit) queue->quit = true;
        else {
          // Independent rect/view channels: coalescing must preserve both, in either arrival order.
          const auto channel = [](Kind kind) {
            return kind == Kind::View ? 4 : kind == Kind::Rect ? 0 : kind == Kind::Show || kind == Kind::Hide ? 1 : kind == Kind::Scale ? 3 : 2;
          };
          const int target = channel(command->kind);
          auto& commands = queue->commands;
          commands.erase(std::remove_if(commands.begin(), commands.end(),
              [&](const Command& old) { return channel(old.kind) == target; }), commands.end());
          queue->commands.push_back(*command);
        }
      }
    });
    s.activation = [NSWorkspace.sharedWorkspace.notificationCenter
        addObserverForName:NSWorkspaceDidActivateApplicationNotification object:nil
        queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification* notification) {
          if (!surface || !surface->embedded) return;
          NSRunningApplication* app = notification.userInfo[NSWorkspaceApplicationKey];
          if (app.processIdentifier == surface->tracker.parent().owner) {
            surface->tracker.owner_activated();
            track(*surface);
          }
        }];
    // The game's process is never the active app (the YouGame app is), so macOS may throttle it as a background app:
    // timers wake late and the GL thread slows down. In the app's full screen that cost the game 20-80% of its frames
    // (observed: "sleeps wake ... late" and gxdecode 3.5 -> 7-9 ms in every full-screen stretch, on time
    // again in page view; ps -M samples: threads at background priority 4 in full screen, 31-54 in page
    // view). A latency-critical activity keeps App Nap and timer coalescing off while the panel exists. It also keeps
    // the display awake: controller input never resets the system's idle timer, so a controller-only player's screen
    // would dim mid-match (the app holds the same for Orca, desktop main.ts). The Mac itself may still sleep when idle.
    s.awake = begin_playing();
    host::log("embed: App Nap off and the display kept awake while the game plays (latency-critical activity)");
    // Proof line: whether macOS counts the panel as visible (it naps an app whose windows are all occluded).
    s.occlusion = [NSNotificationCenter.defaultCenter addObserverForName:NSWindowDidChangeOcclusionStateNotification
        object:s.panel queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification*) {
          if (!surface) return;
          host::log("embed: panel occlusion %s",
                    (surface->panel.occlusionState & NSWindowOcclusionStateVisible) ? "visible" : "occluded");
        }];
    NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
    s.key_on = [center addObserverForName:NSWindowDidBecomeKeyNotification object:s.panel
        queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification*) {
          if (!surface) return;
          if (never_key(*surface)) {
            [surface->panel resignKeyWindow];
            host::log("embed: agent test: neverKey gave the key back");
          }
          focus_changed(*surface, "panel became key");
        }];
    s.key_off = [center addObserverForName:NSWindowDidResignKeyNotification object:s.panel
        queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification*) {
          if (surface) focus_changed(*surface, "panel resigned key");
        }];
    s.app_on = [center addObserverForName:NSApplicationDidBecomeActiveNotification object:NSApp
        queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification*) {
          if (surface) focus_changed(*surface, "game became the active app");
        }];
    s.app_off = [center addObserverForName:NSApplicationDidResignActiveNotification object:NSApp
        queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification*) {
          if (surface) focus_changed(*surface, "game stopped being the active app");
        }];
    // No startup sleeps on the main thread. pump() implements the same 20/50/100/200 ms
    // retry schedule, keeping the SDL event loop and the bridge responsive during startup.
    // set_box_handler just replayed what the app sent before this window existed ('scale' first on 0.13.1076+), so the
    // first placement applies it through pump() instead of a bare track(): never the display-overlap guess when the
    // app already said its scale (d0022 logged one startup 'from display overlap').
    pump();
    return s.sdl;
  }
}
bool active() { return surface && surface->embedded; }
void set_retina(bool enabled) { retina_option.store(enabled); }
bool retina_setting() { return retina_option.load(); }
bool drawable_size(SDL_Window* window, int* w, int* h) {
  std::lock_guard<std::mutex> lock(pixels_mutex);
  return override_size(window && pixels_window == window, drawable_pixels, w, h);
}
bool picture_input(SDL_Window* window, double* x, double* y, int* w, int* h) {
  std::lock_guard<std::mutex> lock(pixels_mutex);
  if (!window || pixels_window != window) return false;
  *x = input_picture.x; *y = input_picture.y;
  *w = picture_points.w; *h = picture_points.h;
  return true;
}
bool picture_hit(SDL_Window* window, double x, double y) {
  std::lock_guard<std::mutex> lock(pixels_mutex);
  return pixels_window != window || (input_shown && contains_point(input_visible, x, y) && contains_point(input_picture, x, y));
}
void attach_metal_view(SDL_Window* window, void* metal_view) {
  const auto attach = ^{
    if (!surface || surface->sdl != window || !metal_view) return;
    // SDL 2.30.9 initially adds this to NSWindow.contentView, even for foreign NSViews.
    NSView* view = static_cast<NSView*>(metal_view);
    [view removeFromSuperview];
    view.frame = surface->picture.bounds;
    view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [surface->picture addSubview:view];
    update_retina(*surface);
  };
  if (NSThread.isMainThread) attach();
  else dispatch_sync(dispatch_get_main_queue(), attach);
}
bool retina_size(SDL_Window* window, int* w, int* h, double* scale) {
  return query_retina_size(retina_launch_enabled, [&] {
    std::lock_guard<std::mutex> lock(pixels_mutex);
    if (!override_size(window && pixels_window == window, pixels_size, w, h)) return false;
    if (scale) *scale = pixels_scale;
    return true;
  });
}
// SDL's GL context may replace/reconfigure the view after create(). Opt in the ACTUAL context view too.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
bool gl_retina_size(SDL_Window* window, int* w, int* h) {
  if (!retina_size(window, w, h)) return false;
  @autoreleasepool {
    NSOpenGLContext* context = NSOpenGLContext.currentContext;
    NSView* view = context.view;
    if (!view) return true;
    if (!view.wantsBestResolutionOpenGLSurface) {
      view.wantsBestResolutionOpenGLSurface = YES;
      [context update];
    }
    // Same AppKit backing query as mac_gl_surface_size(), which remains the presenter's independent diagnostic.
    const NSRect pixels = [view convertRectToBacking:view.bounds];
    *w = int(pixels.size.width); *h = int(pixels.size.height);
    return true;
  }
}
#pragma clang diagnostic pop
bool pump() {
  @autoreleasepool {
    // window_pump calls this for standalone windows too, before the no-surface return.
    mac_host::report_audio_thread_qos();
    if (!surface) return false;
    NSCAssert(NSThread.isMainThread, @"Mac embed commands must run on the main thread");
    auto& s = *surface;
    std::deque<Command> commands;
    bool quit = false;
    {
      std::lock_guard<std::mutex> lock(s.inbox->mutex);
      commands.swap(s.inbox->commands);
      quit = s.inbox->quit;
    }
    if (quit) { hide(s); return true; }
    for (const auto& command : commands) {
      s.presentation.apply(command);
      s.tracker.refresh();
      if (command.kind == Kind::Focus) s.wants_key = true;
      // Blur never calls resignKeyWindow: it only posts the notification (Apple: never call it yourself) and leaves
      // the panel key, so typing on the page died after a match. The app takes the key
      // back itself (win.focus() + webContents.focus()); hide() orders the panel out.
      if (command.kind == Kind::Blur) s.wants_key = false;
    }
    if (frame_window.exchange(nullptr) == s.sdl) s.presentation.first_frame();
#ifdef YOUGAME_AGENT_TEST_HOOK
    agent_actions(s);
#endif
    track(s);
    // Includes hidden panels, native backing/display changes without SDL resize events, and embed scale commands.
    update_retina(s);
    if (now_ms() < s.role_watch_until) {
      StateSample v = sample_ui(s);
      v.role = current_role(TASK_UNSPECIFIED);
      keep_on_screen_role(s, v);
    }
    watch_state(s);
    return false;
  }
}
void frame_presented(SDL_Window* window) { frame_window.store(window); }
bool screenshot_take_requested() { return screenshot_state->slot.requested(); }
uint64_t screenshot_take() { return screenshot_state->slot.take(); }
void screenshot_failed(uint64_t ticket) {
  if (!ticket) return;
  screenshot_state->slot.work(ticket, [=](std::shared_ptr<shot::Request> request) {
    dispatch_group_async(screenshot_group(), screenshot_worker(), ^{
      screenshot_state->slot.finish(request->ticket, now_ms(), shot::error(request->id, "failed"), false, screenshot_emit);
    });
  });
}
static void screenshot_encode(std::shared_ptr<shot::Request> request,
                              int w, int h, size_t pitch, bool bgra, bool bottom_up,
                              std::shared_ptr<const void> owner, const uint8_t* pixels) {
  dispatch_group_async(screenshot_group(), screenshot_worker(), ^{
    // Value captures retain GPU storage and Request, never a renderer/window/Surface.
    (void)owner;
    if (request->cancelled.load()) return;
    std::string reply = shot::error(request->id, "failed");
    bool ok = false;
    try {
      std::vector<uint8_t> rgb;
      const auto size = shot::rgb(rgb, pixels, w, h, pitch, bgra, bottom_up);
      if (request->cancelled.load()) return;
      const auto png = shot::encode(rgb, size);
      if (!png.empty() && !request->cancelled.load()) {
        const char* failure = request->output.write(screenshot_state->data, request->id, png);
        ok = !failure;
        reply = failure ? shot::error(request->id, failure) :
                          shot::success(request->id, shot::path(screenshot_state->data, request->id), png);
      }
    } catch (...) { } // failure still settles the request and removes its own file
    screenshot_state->slot.finish(request->ticket, now_ms(), reply, ok, screenshot_emit);
  });
}
void screenshot_submit(uint64_t ticket, int w, int h, size_t pitch, bool bgra, bool bottom_up,
                       std::shared_ptr<const void> owner, const uint8_t* pixels) {
  screenshot_state->slot.work(ticket, [=](std::shared_ptr<shot::Request> request) {
    screenshot_encode(std::move(request), w, h, pitch, bgra, bottom_up, owner, pixels);
  });
}
void destroy() {
  @autoreleasepool {
    if (screenshot_state->stdio) {
      screenshot_state->slot.stop_accepting(); // also rejects a reader's previously copied callback
      yougame::set_box_handler({});
      screenshot_state->slot.cancel(screenshot_emit); // fail pending/in-flight and unlink our partial output
      // Drain encoding and replies for at most 500 ms. Cancelled workers cannot create
      // an output later, even if an encoder outlives this grace period.
      dispatch_group_wait(screenshot_group(), dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC));
    }
    if (!surface) return;
    { std::lock_guard<std::mutex> lock(pixels_mutex); pixels_window = nullptr; }
    if (!screenshot_state->stdio) yougame::set_box_handler({});
    if (surface->activation)
      [NSWorkspace.sharedWorkspace.notificationCenter removeObserver:surface->activation];
    if (surface->occlusion) [NSNotificationCenter.defaultCenter removeObserver:surface->occlusion];
    for (id observer : {surface->key_on, surface->key_off, surface->app_on, surface->app_off})
      if (observer) [NSNotificationCenter.defaultCenter removeObserver:observer];
    if (surface->awake) {
      [NSProcessInfo.processInfo endActivity:surface->awake];
      [surface->awake release];
    }
    [surface->panel orderOut:nil];
    [surface->panel close];
    [surface->panel release];
    surface.reset();
    frame_window.store(nullptr);
  }
}
} // namespace mac_embed
