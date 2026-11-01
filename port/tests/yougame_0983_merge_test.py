"""v0.9.83 merge boundaries: CLI compatibility, graphics lifecycle, and patch metadata."""
from pathlib import Path
import re
import subprocess
import shutil
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
GIT = '/Library/Developer/CommandLineTools/usr/bin/git' if sys.platform == 'darwin' else (shutil.which('git') or 'git')
CONFLICTED = ('port/app/main.cpp', 'port/linux/renderer_gl.cpp', 'port/linux/window_sdl.inl',
              'port/runtime/gx/gx_d3d11.cpp', 'port/runtime/gx/gx_d3d12.cpp',
              'port/runtime/gx/pc_settings.cpp', 'port/runtime/host/window.cpp', 'port/runtime/host/window.h')


def source(path):
    return (ROOT / path).read_text(encoding='utf-8-sig')


def revision(ref, path):
    if not (ROOT / '.git').exists():
        raise unittest.SkipTest('history checks require a git checkout')
    available = subprocess.run([GIT, 'show', '--format=', '--no-patch', ref + '^{commit}'], cwd=ROOT,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if available.returncode:
        raise unittest.SkipTest('history checks require revision ' + ref)
    return subprocess.check_output([GIT, 'show', ref + ':' + path], cwd=ROOT).decode('utf-8-sig')


def flags(text):
    return set(re.findall(r'a == "(--[a-z0-9-]+)"', text))


class YouGame0983MergeTest(unittest.TestCase):
    def test_all_conflict_markers_are_removed(self):
        for path in CONFLICTED:
            self.assertIsNone(re.search(r'^(?:<{7}|={7}|>{7}|\|{7})(?: |$)', source(path), re.M), path)

    def test_upstream_cli_delta_and_existing_options_survive(self):
        path = 'port/app/main.cpp'
        before, upstream, merged = (flags(revision('v0.9.82', path)),
                                     flags(revision('986f971e', path)), flags(source(path)))
        self.assertEqual(upstream - before, {'--msaa'})
        self.assertEqual(before - upstream, set())
        self.assertTrue(flags(revision('HEAD', path)) <= merged)
        self.assertTrue(upstream <= merged)
        main = source(path)
        self.assertIn('v == "vulkan" || v == "vk"', main)
        self.assertIn('gfx.ssaa = std::clamp(n, 1, 4)', main)
        # The launch spec in mu-ci and mu-ci-console has these ten switches.
        for flag in ('--iso', '--threaded-renderer', '--settings-path', '--sys-dir', '--user-dir',
                     '--replay-dir', '--card-dir', '--log-file', '--shader-cache', '--vanilla-only'):
            self.assertIn('"' + flag + '"', main)

    def test_vulkan_window_selection_precedes_creation_in_both_paths(self):
        for path, prefix in (('port/app/main.cpp', 'gfx'), ('port/runtime/gx/threaded_backend.cpp', 'options')):
            text = source(path)
            prefer = text.index('host::window_prefer_vulkan(' + prefix + '.api')
            self.assertLess(text.index('vulkan_available()', text.rfind('if (', 0, prefer) - 300), prefer)
            self.assertLess(prefer, text.index('host::window_create(', prefer))
        window = source('port/linux/window_sdl.inl')
        self.assertIn('(g_vulkan_window ? SDL_WINDOW_VULKAN : SDL_WINDOW_OPENGL)', window)
        self.assertIn('flags |= boxed ? SDL_WINDOW_HIDDEN', window)
        header = source('port/runtime/host/window.h')
        self.assertLess(header.index('#endif', header.index('void* window_create')),
                        header.index('void window_prefer_vulkan'))

    def test_mac_keeps_gl_and_vulkan_free_build(self):
        vulkan = source('port/runtime/gx/gx_vulkan.cpp')
        mac = vulkan.split('#ifdef __APPLE__', 1)[1].split('#else', 1)[0]
        self.assertIn('bool vulkan_available() { return false; }', mac)
        self.assertNotIn('#include <vulkan/', mac)
        backend = source('port/linux/full/gx_backend_linux.cpp')
        self.assertIn('#ifndef __APPLE__\n#include <SDL_vulkan.h>\n#endif', backend)
        self.assertIn('#ifndef __APPLE__\n  // Vulkan', backend)
        self.assertIn('if (host::window_embedded()) host::window_client_size(w, h);', backend)

    def test_imgui_chooses_backend_and_preserves_manual_gamepads(self):
        settings = source('port/runtime/gx/pc_settings.cpp')
        start = settings.index('if (SDL_GetWindowFlags(')
        end = settings.index('io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;', start)
        block = settings[start:end]
        for call in ('ImGui_ImplSDL2_InitForVulkan', 'ImGui_ImplSDL2_InitForOpenGL',
                     'ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_Manual, nullptr, 0)'):
            self.assertIn(call, block)

    def test_gl_msaa_preserves_state_cache_and_embed_evidence(self):
        from yougame_console_wiring_test import body
        gl = source('port/linux/renderer_gl.cpp')
        self.assertIn('invalidate_state();', body(gl, 'apply_msaa'))
        draw = body(gl, 'draw')
        self.assertIn('use_program(p); attributes(p); glBindFramebuffer(GL_FRAMEBUFFER, draw_fbo());', draw)
        present = body(gl, 'present')
        self.assertLess(present.index('resolve_msaa();'), present.index('picture(c,'))
        for token in ('hooks_.drawable_size', 'test_shot(w, h)', 'evidence.after_swap(',
                      'hooks_.after_swap(presented_)', 'yougame::parity::signals.presented(test_fill < 0)'):
            self.assertIn(token, present)
        self.assertIn('bind_vertex_array(vao_);', gl)

    def test_metal_ready_requires_successful_real_present(self):
        from yougame_console_wiring_test import body
        metal = source('port/mac/gx_metal.mm')
        self.assertIn('#include "../runtime/hle/yougame_parity.h"', metal)
        present = body(metal, 'MetalBackend::present')
        self.assertIn('if (w <= 0 || h <= 0)', present)
        self.assertIn('if (!drawable)', present)
        callback = '''  const bool real_frame = frame.test_fill < 0;
  [drawable addPresentedHandler:^(id<MTLDrawable> presented) {
    if (presented.presentedTime > 0)
      yougame::parity::signals.presented(real_frame);
  }];'''
        self.assertIn(callback, present)
        self.assertLess(present.index('if (!drawable)'), present.index(callback))
        self.assertLess(present.index(callback), present.index('[command_.get() presentDrawable:drawable]'))
        self.assertEqual(metal.count('yougame::parity::signals.presented('), 1)
        # Like GL, use presented()'s default Clock::now() under the agent hook.
        self.assertIn('Clock::time_point now = Clock::now()', source('port/runtime/hle/yougame_parity.h'))
        submit = body(metal, 'MetalBackend::submit_frame')
        self.assertLess(submit.index('if (!skip_) present(frame, efb)'), submit.index('[command_.get() commit]'))

    def test_dxgi_probes_cannot_release_cover_or_pollute_present_metrics(self):
        for api in (11, 12):
            text = source(f'port/runtime/gx/gx_d3d{api}.cpp')
            self.assertIn('&& !yougame::embedded();', text)
            self.assertIn('if (test_only) return swapchain_->Present(0, DXGI_PRESENT_TEST);', text)
            self.assertIn('signals.presented(!test_only && SUCCEEDED(present_result) && frame.test_fill < 0)', text)
            self.assertIn('if (!test_only) pacing_diagnostics_.present(', text)
            self.assertIn('return swapchain_->Present(present.interval, present.flags);', text)
        self.assertIn('occluded_ && fg_applied_ <= 0 && !yougame::embedded()', source('port/runtime/gx/gx_d3d12.cpp'))

    def test_vulkan_ready_requires_successful_real_present(self):
        text = source('port/runtime/gx/gx_vulkan.cpp')
        self.assertIn('end_frame(presented, frame.test_fill < 0)', text)
        start = text.index('const VkResult r = vkQueuePresentKHR')
        tail = text[start:text.index('acquired_ = false;', start)]
        self.assertIn('if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR)', tail)
        self.assertIn('yougame::parity::signals.presented(real_frame)', tail)
        self.assertIn('host::window_presented(presented_)', tail)
        self.assertIn('#ifdef MELEE_NO_SLIPPI\n#include "netplay_state.h"', text)

    def test_patch_new_offsets_follow_hunk_deltas(self):
        patch = source('sourceport/patches/melee-native.patch')
        count = 0
        for section in patch.split('diff --git ')[1:]:
            delta = 0
            for m in re.finditer(r'^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@', section, re.M):
                old, old_n, new, new_n = (int(m[1]), int(m[2] or 1), int(m[3]), int(m[4] or 1))
                self.assertEqual(new, old + delta + (old_n == 0) - (new_n == 0), section.splitlines()[0])
                delta += new_n - old_n
                count += 1
        # Build 28 plus upstream v0.9.87 has 10067 hunks.
        self.assertEqual(count, 10067)


if __name__ == '__main__':
    unittest.main()
