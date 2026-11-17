"""Mac embed clipping contract, checked without AppKit initialization or a linked binary."""
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]


def source(path):
    return (ROOT / path).read_text()


class MacEmbedViewTest(unittest.TestCase):
    def test_view_validation_and_independent_inbox_channel(self):
        state = source('port/mac/embed_mac_state.h')
        self.assertIn('view ? Kind::View : Kind::Rect', state)
        self.assertIn('(!view && line.rfind("rect ", 0) != 0)', state)
        self.assertIn('line.substr(5)', state)
        self.assertIn('w < 0 || h < 0 || w > limit || h > limit', state)
        adapter = source('port/mac/embed_mac.mm')
        self.assertIn('kind == Kind::View ? 4 : kind == Kind::Rect ? 0', adapter)
        self.assertIn('auto& last = command->kind == Kind::View ? last_view : last_rect;', adapter)

    def test_drawable_is_inner_view_and_metal_is_reparented(self):
        adapter = source('port/mac/embed_mac.mm')
        self.assertIn('SDL_CreateWindowFrom(s.picture)', adapter)
        self.assertIn('clip.clipsToBounds = YES;', adapter)
        self.assertIn('clip.autoresizesSubviews = NO;', adapter)
        self.assertIn('s.picture.autoresizingMask = NSViewNotSizable;', adapter)
        self.assertIn('std::round(picture_frame.w), std::round(picture_frame.h)', adapter)
        self.assertIn('view.frame = surface->picture.bounds;', adapter)
        self.assertIn('[surface->picture addSubview:view];', adapter)
        self.assertIn('mac_embed::attach_metal_view(window_, view_.get());', source('port/mac/gx_metal.mm'))
        host = source('port/linux/window_sdl.inl')
        self.assertIn('mac_embed::active() && w == g_client_w && h == g_client_h', host)
        self.assertIn('mac_embed::drawable_size(sdl_window(), &retina_w, &retina_h)', host)

    def test_hit_testing_and_ui_use_picture_origin_and_clip(self):
        adapter = source('port/mac/embed_mac.mm')
        self.assertIn('NSPointInRect([self convertPoint:point fromView:self.superview], self.bounds)', adapter)
        self.assertIn('contains_point(input_visible, x, y) && contains_point(input_picture, x, y)', adapter)
        ui = source('port/mac/imgui_retina.h')
        self.assertIn('(e.MousePos.PosX - origin.x) * scale.x', ui)
        self.assertIn('!picture_hit(window, event.button.x, event.button.y)', ui)
        self.assertIn('e.MousePos.PosX = e.MousePos.PosY = -FLT_MAX;', ui)
        self.assertIn('if (drawable_size(window, &w, &h)) ImGui::GetIO().DisplaySize', ui)

    def test_metal_drawable_follows_picture_on_main_thread_without_window_event(self):
        adapter = source('port/mac/embed_mac.mm')
        update = adapter.split('void update_retina(Surface& s) {', 1)[1].split('// Only IDs', 1)[0]
        self.assertIn('NSCAssert(NSThread.isMainThread', update)
        self.assertIn('NSView* view = s.picture;', update)
        self.assertIn('s.embedded && s.retina ? scale : 1', update)
        self.assertIn('backing_size(view.bounds.size.width, view.bounds.size.height, drawable_scale)', update)
        self.assertIn('child.frame = view.bounds;', update)
        self.assertIn('layer.contentsScale = drawable_scale;', update)
        self.assertIn('CGSizeMake(drawable.w, drawable.h)', update)
        self.assertIn('layer.drawableSize = pixels;', update)
        self.assertIn('drawable_pixels = s.retina ? size : picture_points;', update)
        attach = adapter.split('void attach_metal_view(', 1)[1].split('bool retina_size(', 1)[0]
        self.assertIn('update_retina(*surface);', attach)
        self.assertIn('dispatch_sync(dispatch_get_main_queue(), attach)', attach)
        pump = adapter.split('bool pump() {', 1)[1].split('void frame_presented(', 1)[0]
        self.assertLess(pump.index('track(s);'), pump.index('update_retina(s);'))
        present = source('port/mac/gx_metal.mm').split('void MetalBackend::present(', 1)[1].split('const uint64_t screenshot', 1)[0]
        self.assertIn('const bool embedded = mac_embed::drawable_size(window_, &w, &h);', present)
        self.assertIn('if (!embedded) SDL_Metal_GetDrawableSize(window_, &w, &h);', present)
        self.assertIn('if (!embedded) layer_.drawableSize = CGSizeMake(w, h);', present)
        self.assertNotIn('retina_size(', present)
        self.assertNotIn('SDL_GetWindowSize(', present)

    def test_metal_projection_and_presentation_use_picture_in_both_retina_modes(self):
        metal = source('port/mac/gx_metal.mm')
        projection = metal.split('void MetalBackend::update_widescreen() {', 1)[1].split('void MetalBackend::update_embed_scale()', 1)[0]
        self.assertIn('if (!mac_embed::drawable_size(window_, &width, &height)) SDL_GetWindowSize(window_, &width, &height);', projection)
        self.assertIn('true_widescreen_aspect(options_, width, height)', projection)
        self.assertNotIn('retina_size(', projection)
        present = metal.split('void MetalBackend::present(', 1)[1].split('void MetalBackend::', 1)[0]
        self.assertLess(present.index('mac_embed::drawable_size(window_, &w, &h)'),
                        present.index('presented_aspect(options_, w, h, false)'))

    def test_gl_projection_and_presentation_follow_picture_not_clip(self):
        host = source('port/linux/window_sdl.inl')
        pump = host.split('void window_pump() {', 1)[1]
        self.assertIn('mac_embed::drawable_size(sdl_window(), &retina_w, &retina_h)', pump)
        self.assertIn('g_client_w = retina_w; g_client_h = retina_h;', pump)
        self.assertIn('void window_client_size(int* w, int* h)', host)
        self.assertIn('*w = g_client_w; *h = g_client_h;', host)
        gl_host = source('port/linux/full/gx_backend_linux.cpp')
        projection = gl_host.split('hooks.frame_begin =', 1)[1].split('hooks.before_swap =', 1)[0]
        self.assertIn('host::window_client_size(&cw, &ch);', projection)
        self.assertIn('true_widescreen_aspect(p.options, cw, ch)', projection)
        self.assertIn('presented_aspect(p.options, w, h, false)', projection)
        gl = source('port/linux/renderer_gl.cpp')
        present = gl.split('void present(const gx::EfbCopy& c, const gx::Frame& frame) {', 1)[1]
        # SDL wraps s.picture (pinned above); its GL pixel query uses that view's bounds even at 1x.
        self.assertIn('SDL_GL_GetDrawableSize(window_, &w, &h);', present)
        self.assertLess(present.index('mac_embed::gl_retina_size(window_, &w, &h);'),
                        present.index('gx::presented_aspect(*hooks_.options, w, h, widenable_scene_)'))
        adapter = source('port/mac/embed_mac.mm')
        gl_size = adapter.split('bool gl_retina_size(', 1)[1].split('#pragma clang diagnostic pop', 1)[0]
        self.assertIn('NSView* view = context.view;', gl_size)
        self.assertIn('[view convertRectToBacking:view.bounds]', gl_size)
        self.assertNotIn('panel', gl_size)

    def test_policy_at_compile_time_without_link_or_execution(self):
        compiler = shutil.which('clang++')
        if not compiler:
            self.skipTest('local clang++ missing; no download permitted')
        policy = r'''
#include "port/mac/embed_mac_state.h"
using namespace mac_embed;
constexpr bool clipping() {
  Presentation p;
  Parent parent{true, true};
  p.first_frame();
  p.apply({Kind::Rect, {10, 20, 800, 600}});
  if (!equal(p.picture(), p.box) || !p.visible(parent)) return false;
  p.apply({Kind::View, {10, 20, 800, 600}});
  const auto fixed = p.picture();
  for (double scale : {1.0, 1.5, 2.0}) {
    p.apply({Kind::Scale, {}, scale});
    for (auto clip : {Rect{110, 80, 600, 400}, Rect{-50, -50, 1000, 1000}, Rect{0, 0, 0, 0}}) {
      p.apply({Kind::Rect, clip});
      if (!equal(p.picture(), fixed)) return false;
      const auto points = points_from_backing(p.picture(), p.app_scale);
      if (points.w * scale != 800 || points.h * scale != 600) return false;
    }
  }
  if (p.visible(parent)) return false;
  p.apply({Kind::Rect, {-50, -50, 1000, 1000}});
  if (!equal(p.visible_box(), fixed)) return false;
  p.apply({Kind::Rect, {110, 80, 600, 400}});
  if (!contains_point(p.visible_box(), 110, 80) || contains_point(p.visible_box(), 109, 80) ||
      contains_point(p.visible_box(), 710, 80)) return false;
  p.apply({Kind::Hide}); p.apply({Kind::Rect, {10, 20, 800, 600}});
  if (p.visible(parent)) return false;
  Presentation first;
  first.first_frame(); first.apply({Kind::View, fixed});
  if (first.visible(parent)) return false;
  first.apply({Kind::Rect, {110, 80, 600, 400}});
  return first.visible(parent) && equal(first.picture(), fixed);
}
static_assert(clipping(), "picture, scale, legacy, clip/input, hide, ordering contract");
constexpr bool view_only_resize_and_aspect() {
  Presentation p;
  p.apply({Kind::Rect, {0, 0, 800, 600}});
  p.apply({Kind::View, {0, 0, 800, 600}});
  p.apply({Kind::View, {0, 0, 1600, 1200}});
  if (p.box.w != 800 || p.box.h != 600 || p.picture().w != 1600 || p.picture().h != 1200) return false;
  p.apply({Kind::View, {0, 0, 1600, 900}});
  for (double scale : {1.0, 1.5, 2.0}) {
    p.apply({Kind::Scale, {}, scale});
    for (auto clip : {Rect{0, 0, 1200, 900}, Rect{100, 40, 800, 600}}) {
      p.apply({Kind::Rect, clip});
      const auto points = points_from_backing(p.picture(), p.app_scale);
      if (points.w * 9 != points.h * 16 || points.w * scale != 1600 || points.h * scale != 900) return false;
    }
  }
  return true;
}
static_assert(view_only_resize_and_aspect(), "view-only resize and clip-independent Fit window at 1x/Retina");
'''
        run = subprocess.run([compiler, '-std=c++17', '-fsyntax-only', '-x', 'c++', '-iquote', str(ROOT), '-'],
                             input=policy, text=True, capture_output=True, timeout=30)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == '__main__':
    unittest.main()
